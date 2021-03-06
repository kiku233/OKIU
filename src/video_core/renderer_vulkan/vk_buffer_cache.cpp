// Copyright 2019 yuzu Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <algorithm>
#include <cstring>
#include <memory>

#include "core/core.h"
#include "video_core/buffer_cache/buffer_cache.h"
#include "video_core/renderer_vulkan/vk_buffer_cache.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_stream_buffer.h"
#include "video_core/vulkan_common/vulkan_device.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"

namespace Vulkan {

namespace {

constexpr VkBufferUsageFlags BUFFER_USAGE =
    VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
    VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;

constexpr VkPipelineStageFlags UPLOAD_PIPELINE_STAGE =
    VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_VERTEX_INPUT_BIT |
    VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;

constexpr VkAccessFlags UPLOAD_ACCESS_BARRIERS =
    VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_UNIFORM_READ_BIT |
    VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_INDEX_READ_BIT;

constexpr VkAccessFlags TRANSFORM_FEEDBACK_WRITE_ACCESS =
    VK_ACCESS_TRANSFORM_FEEDBACK_WRITE_BIT_EXT | VK_ACCESS_TRANSFORM_FEEDBACK_COUNTER_WRITE_BIT_EXT;

std::unique_ptr<VKStreamBuffer> CreateStreamBuffer(const Device& device, VKScheduler& scheduler) {
    return std::make_unique<VKStreamBuffer>(device, scheduler);
}

} // Anonymous namespace

Buffer::Buffer(const Device& device_, MemoryAllocator& memory_allocator, VKScheduler& scheduler_,
               StagingBufferPool& staging_pool_, VAddr cpu_addr_, std::size_t size_)
    : BufferBlock{cpu_addr_, size_}, device{device_}, scheduler{scheduler_}, staging_pool{
                                                                                 staging_pool_} {
    buffer = device.GetLogical().CreateBuffer(VkBufferCreateInfo{
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .size = static_cast<VkDeviceSize>(size_),
        .usage = BUFFER_USAGE | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .queueFamilyIndexCount = 0,
        .pQueueFamilyIndices = nullptr,
    });
    commit = memory_allocator.Commit(buffer, MemoryUsage::DeviceLocal);
}

Buffer::~Buffer() = default;

void Buffer::Upload(std::size_t offset, std::size_t data_size, const u8* data) {
    const auto& staging = staging_pool.Request(data_size, MemoryUsage::Upload);
    std::memcpy(staging.mapped_span.data(), data, data_size);

    scheduler.RequestOutsideRenderPassOperationContext();

    const VkBuffer handle = Handle();
    scheduler.Record([staging = staging.buffer, handle, offset, data_size,
                      &device = device](vk::CommandBuffer cmdbuf) {
        const VkBufferMemoryBarrier read_barrier{
            .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
            .pNext = nullptr,
            .srcAccessMask =
                VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT |
                VK_ACCESS_HOST_WRITE_BIT |
                (device.IsExtTransformFeedbackSupported() ? TRANSFORM_FEEDBACK_WRITE_ACCESS : 0),
            .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .buffer = handle,
            .offset = offset,
            .size = data_size,
        };
        const VkBufferMemoryBarrier write_barrier{
            .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
            .pNext = nullptr,
            .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .dstAccessMask = UPLOAD_ACCESS_BARRIERS,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .buffer = handle,
            .offset = offset,
            .size = data_size,
        };
        cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                               0, read_barrier);
        cmdbuf.CopyBuffer(staging, handle, VkBufferCopy{0, offset, data_size});
        cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT, UPLOAD_PIPELINE_STAGE, 0,
                               write_barrier);
    });
}

void Buffer::Download(std::size_t offset, std::size_t data_size, u8* data) {
    auto staging = staging_pool.Request(data_size, MemoryUsage::Download);
    scheduler.RequestOutsideRenderPassOperationContext();

    const VkBuffer handle = Handle();
    scheduler.Record(
        [staging = staging.buffer, handle, offset, data_size](vk::CommandBuffer cmdbuf) {
            const VkBufferMemoryBarrier barrier{
                .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
                .pNext = nullptr,
                .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
                .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .buffer = handle,
                .offset = offset,
                .size = data_size,
            };

            cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_VERTEX_SHADER_BIT |
                                       VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                                       VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                   VK_PIPELINE_STAGE_TRANSFER_BIT, 0, {}, barrier, {});
            cmdbuf.CopyBuffer(handle, staging, VkBufferCopy{offset, 0, data_size});
        });
    scheduler.Finish();

    std::memcpy(data, staging.mapped_span.data(), data_size);
}

void Buffer::CopyFrom(const Buffer& src, std::size_t src_offset, std::size_t dst_offset,
                      std::size_t copy_size) {
    scheduler.RequestOutsideRenderPassOperationContext();

    const VkBuffer dst_buffer = Handle();
    scheduler.Record([src_buffer = src.Handle(), dst_buffer, src_offset, dst_offset,
                      copy_size](vk::CommandBuffer cmdbuf) {
        cmdbuf.CopyBuffer(src_buffer, dst_buffer, VkBufferCopy{src_offset, dst_offset, copy_size});

        std::array<VkBufferMemoryBarrier, 2> barriers;
        barriers[0].sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
        barriers[0].pNext = nullptr;
        barriers[0].srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        barriers[0].dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        barriers[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barriers[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barriers[0].buffer = src_buffer;
        barriers[0].offset = src_offset;
        barriers[0].size = copy_size;
        barriers[1].sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
        barriers[1].pNext = nullptr;
        barriers[1].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barriers[1].dstAccessMask = UPLOAD_ACCESS_BARRIERS;
        barriers[1].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barriers[1].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barriers[1].buffer = dst_buffer;
        barriers[1].offset = dst_offset;
        barriers[1].size = copy_size;
        cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT, UPLOAD_PIPELINE_STAGE, 0, {},
                               barriers, {});
    });
}

VKBufferCache::VKBufferCache(VideoCore::RasterizerInterface& rasterizer_,
                             Tegra::MemoryManager& gpu_memory_, Core::Memory::Memory& cpu_memory_,
                             const Device& device_, MemoryAllocator& memory_allocator_,
                             VKScheduler& scheduler_, VKStreamBuffer& stream_buffer_,
                             StagingBufferPool& staging_pool_)
    : VideoCommon::BufferCache<Buffer, VkBuffer, VKStreamBuffer>{rasterizer_, gpu_memory_,
                                                                 cpu_memory_, stream_buffer_},
      device{device_}, memory_allocator{memory_allocator_}, scheduler{scheduler_},
      staging_pool{staging_pool_} {}

VKBufferCache::~VKBufferCache() = default;

std::shared_ptr<Buffer> VKBufferCache::CreateBlock(VAddr cpu_addr, std::size_t size) {
    return std::make_shared<Buffer>(device, memory_allocator, scheduler, staging_pool, cpu_addr,
                                    size);
}

VKBufferCache::BufferInfo VKBufferCache::GetEmptyBuffer(std::size_t size) {
    size = std::max(size, std::size_t(4));
    const auto& empty = staging_pool.Request(size, MemoryUsage::DeviceLocal);
    scheduler.RequestOutsideRenderPassOperationContext();
    scheduler.Record([size, buffer = empty.buffer](vk::CommandBuffer cmdbuf) {
        cmdbuf.FillBuffer(buffer, 0, size, 0);
    });
    return {empty.buffer, 0, 0};
}

} // namespace Vulkan
