// Copyright 2018 yuzu emulator team
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <cstring>

#include "common/assert.h"
#include "common/logging/log.h"
#include "common/scope_exit.h"
#include "core/core.h"
#include "core/hle/service/nvdrv/devices/nvhost_vic.h"
#include "core/hle/service/nvdrv/devices/nvmap.h"
#include "video_core/gpu.h"
#include "video_core/memory_manager.h"

namespace Service::Nvidia::Devices {

namespace NvErrCodes {
constexpr u32 Success{};
constexpr u32 OutOfMemory{static_cast<u32>(-12)};
constexpr u32 InvalidInput{static_cast<u32>(-22)};
} // namespace NvErrCodes

nvhost_vic::nvhost_vic(Core::System& system, std::shared_ptr<nvmap> nvmap_dev)
    : nvdevice(system), nvmap_dev(std::move(nvmap_dev)) {}
nvhost_vic::~nvhost_vic() = default;

u32 nvhost_vic::ioctl(Ioctl command, const std::vector<u8>& input, const std::vector<u8>& input2,
                      std::vector<u8>& output, std::vector<u8>& output2, IoctlCtrl& ctrl,
                      IoctlVersion version) {
    LOG_DEBUG(Service_NVDRV, "called, command=0x{:08X}, input_size=0x{:X}, output_size=0x{:X}",
              command.raw, input.size(), output.size());

    if (system.GPU().UseNvdec()) {
        switch (static_cast<IoctlCommand>(command.raw)) {
        case IoctlCommand::IocSetNVMAPfdCommand:
            return SetNVMAPfd(input, output);
        case IoctlCommand::IocSubmit:
            return Submit(input, output);
        case IoctlCommand::IocGetSyncpoint:
            return GetSyncpoint(input, output);
        case IoctlCommand::IocGetWaitbase:
            return GetWaitbase(input, output);
        case IoctlCommand::IocMapBuffer:
            return MapBuffer(input, output);
        case IoctlCommand::IocMapBufferEx:
            return MapBuffer(input, output);
        case IoctlCommand::IocUnmapBufferEx:
            return UnmapBuffer(input, output);
        }
        if (command.group == 0) {
            if (command.cmd == 9) {
                return MapBuffer(input, output);
            }
            if (command.cmd == 0xa) {
                return UnmapBuffer(input, output);
            }
        }
        if (command.group == 0) {
            if (command.cmd == 1) {
                return Submit(input, output);
            }
        }
    } else {
        // Fall back to stubbed management if user disables nvdec
        // TODO: Fix crashing that is caused in some instances due to ioctl commands
        switch (static_cast<IoctlCommand>(command.raw)) {
        case IoctlCommand::IocSetNVMAPfdCommand:
            return SetNVMAPfd(input, output);
        case IoctlCommand::IocSubmit:
            return SubmitStub(input, output);
        case IoctlCommand::IocGetSyncpoint:
            return GetSyncpoint(input, output);
        case IoctlCommand::IocGetWaitbase:
            return GetWaitbase(input, output);
        case IoctlCommand::IocMapBuffer:
            return MapBufferStub(input, output);
        case IoctlCommand::IocMapBufferEx:
            return MapBufferStub(input, output);
        case IoctlCommand::IocUnmapBufferEx:
            return UnmapBufferEx(input, output);
        }
    }
    UNIMPLEMENTED_MSG("Unimplemented ioctl 0x{:X}", command.raw);
    return 0;
}

u32 nvhost_vic::SetNVMAPfd(const std::vector<u8>& input, std::vector<u8>& output) {
    IoctlSetNvmapFD params{};
    std::memcpy(&params, input.data(), sizeof(IoctlSetNvmapFD));
    LOG_DEBUG(Service_NVDRV, "called, fd={}", params.nvmap_fd);

    nvmap_fd = params.nvmap_fd;
    return 0;
}

// TODO(ameerj): move overlapping functionality into an nvdec_common

// Splice vectors will copy count amount of type T from the input vector into the dst vector.
template <class T>
std::size_t splice_vectors(const std::vector<u8>& input, std::vector<T>& dst, std::size_t count,
                           std::size_t offset) {
    std::memcpy(dst.data(), input.data() + offset, count * sizeof(T));
    return offset += count * sizeof(T);
}

// Write vectors will write data to the output buffer
template <class T>
std::size_t write_vectors(std::vector<u8>& dst, std::vector<T>& src, std::size_t offset) {
    std::memcpy(dst.data() + offset, src.data(), src.size() * sizeof(T));
    return offset += src.size() * sizeof(T);
}

u32 nvhost_vic::Submit(const std::vector<u8>& input, std::vector<u8>& output) {
    IoctlSubmit params{};
    std::memcpy(&params, input.data(), sizeof(IoctlSubmit));
    LOG_DEBUG(Service_NVDRV, "called NVDEC Submit, cmd_buffer_count={}", params.cmd_buffer_count);

    // Instantiate param buffers
    // TODO(ameerj): utilize async gpu related buffers
    std::size_t offset = sizeof(IoctlSubmit);
    std::vector<CommandBuffer> command_buffers(params.cmd_buffer_count);
    // std::vector<Reloc> relocs(params.relocation_count);
    // std::vector<u32> reloc_shifts(params.relocation_count);
    // std::vector<SyncptIncr> syncpt_increments(params.syncpoint_count);
    // std::vector<SyncptIncr> wait_checks(params.syncpoint_count);
    // std::vector<Fence> fences(params.fence_count);

    // splice input into their respective buffers
    offset = splice_vectors(input, command_buffers, params.cmd_buffer_count, offset);
    // offset = splice_vectors(input, relocs, params.relocation_count, offset);
    // offset = splice_vectors(input, reloc_shifts, params.relocation_count, offset);
    // offset = splice_vectors(input, syncpt_increments, params.syncpoint_count, offset);
    // offset = splice_vectors(input, wait_checks, params.syncpoint_count, offset);
    // offset = splice_vectors(input, fences, params.fence_count, offset);

    auto& gpu = system.GPU();

    // TODO: Use fences for async gpu syncpoint management

    for (const auto& cmd_buffer : command_buffers) {
        auto object = nvmap_dev->GetObject(cmd_buffer.memory_id);
        ASSERT(object);
        const auto map = FindBufferMap(object->dma_map_addr);
        if (!map) {
            LOG_ERROR(Service_NVDRV, "Tried to submit an invalid offset 0x{:X} dma 0x{:X}",
                      object->addr, object->dma_map_addr);
            return 0;
        }
        Tegra::ChCommandHeaderList cmdlist(cmd_buffer.word_count);
        gpu.MemoryManager().ReadBlock(map->StartAddr() + cmd_buffer.offset, cmdlist.data(),
                                      cmdlist.size() * sizeof(u32));
        gpu.PushCommandBuffer(cmdlist);
    }
    std::memcpy(output.data(), &params, sizeof(IoctlSubmit));
    // Some games expect command_buffers to be written back
    offset = write_vectors(output, command_buffers, sizeof(IoctlSubmit));
    return NvErrCodes::Success;
}

u32 nvhost_vic::GetSyncpoint(const std::vector<u8>& input, std::vector<u8>& output) {
    IoctlGetSyncpoint params{};
    std::memcpy(&params, input.data(), input.size());
    LOG_DEBUG(Service_NVDRV, "called GetSyncpoint, id={}", params.param);

    // We found that this causes deadlocks with async gpu, along with degraded performance.
    // TODO: RE the nvdec async implementation
    params.value = 0;
    std::memcpy(output.data(), &params, sizeof(IoctlGetSyncpoint));

    return NvErrCodes::Success;
}

u32 nvhost_vic::GetWaitbase(const std::vector<u8>& input, std::vector<u8>& output) {
    IoctlGetWaitbase params{};
    std::memcpy(&params, input.data(), sizeof(IoctlGetWaitbase));
    LOG_INFO(Service_NVDRV, "called GetWaitbase, unknown=0x{:X}", params.unknown);
    params.value = 0; // Seems to be hard coded at 0
    std::memcpy(output.data(), &params, sizeof(IoctlGetWaitbase));
    return 0;
}

u32 nvhost_vic::MapBuffer(const std::vector<u8>& input, std::vector<u8>& output) {
    IoctlMapBuffer params{};
    std::memcpy(&params, input.data(), sizeof(IoctlMapBuffer));
    std::vector<MapBufferEntry> cmd_buffer_handles(params.num_entries);

    splice_vectors(input, cmd_buffer_handles, params.num_entries, sizeof(IoctlMapBuffer));

    auto& gpu = system.GPU();

    for (auto& cmf_buff : cmd_buffer_handles) {
        auto object{nvmap_dev->GetObject(cmf_buff.map_handle)};
        if (!object) {
            LOG_ERROR(Service_NVDRV, "invalid cmd_buffer nvmap_handle={:X}", cmf_buff.map_handle);
            std::memcpy(output.data(), &params, output.size());
            return NvErrCodes::InvalidInput;
        }

        if (object->dma_map_addr == 0) {
            // We are mapping in the lower 32-bit address space
            GPUVAddr low_addr = gpu.MemoryManager().MapLow(object->addr, object->size);
            object->dma_map_addr = static_cast<u32>(low_addr);
            ASSERT(object->dma_map_addr == low_addr);
        }

        if (!object->dma_map_addr) {
            LOG_ERROR(Service_NVDRV, "failed to map size={}", object->size);
        } else {
            // TODO(ameerj): Forgo address pinning in favor of using the MapLow values directly
            cmf_buff.map_address =
                gpu.MemoryManager().PinAddress(object->dma_map_addr, object->size);
            AddBufferMap(object->dma_map_addr, object->size, object->addr,
                         object->status == nvmap::Object::Status::Allocated, cmf_buff.map_address);
        }
    }
    std::memcpy(output.data(), &params, sizeof(IoctlMapBuffer));
    std::memcpy(output.data() + sizeof(IoctlMapBuffer), cmd_buffer_handles.data(),
                cmd_buffer_handles.size() * sizeof(MapBufferEntry));

    return NvErrCodes::Success;
}

u32 nvhost_vic::UnmapBuffer(const std::vector<u8>& input, std::vector<u8>& output) {
    IoctlMapBuffer params{};
    std::memcpy(&params, input.data(), sizeof(IoctlMapBuffer));
    std::vector<MapBufferEntry> cmd_buffer_handles(params.num_entries);
    splice_vectors(input, cmd_buffer_handles, params.num_entries, sizeof(IoctlMapBuffer));

    auto& gpu = system.GPU();

    for (auto& cmf_buff : cmd_buffer_handles) {
        const auto object{nvmap_dev->GetObject(cmf_buff.map_handle)};
        if (!object) {
            LOG_ERROR(Service_NVDRV, "invalid cmd_buffer nvmap_handle={:X}", cmf_buff.map_handle);
            std::memcpy(output.data(), &params, output.size());
            return NvErrCodes::InvalidInput;
        }

        if (const auto size{RemoveBufferMap(object->dma_map_addr)}; size) {
            gpu.MemoryManager().UnpinAddress(object->dma_map_addr);
            gpu.MemoryManager().Unmap(object->dma_map_addr, *size);
            object->dma_map_addr = 0;
        } else {
            LOG_DEBUG(Service_NVDRV, "invalid offset=0x{:X} dma=0x{:X}", object->addr,
                      object->dma_map_addr);
        }
    }
    std::memset(output.data(), 0, output.size());
    return NvErrCodes::Success;
}

u32 nvhost_vic::MapBufferEx(const std::vector<u8>& input, std::vector<u8>& output) {
    IoctlMapBufferEx params{};
    std::memcpy(&params, input.data(), sizeof(IoctlMapBufferEx));
    LOG_WARNING(Service_NVDRV, "(STUBBED) called with address={:08X}{:08X}", params.address_2,
                params.address_1);
    params.address_1 = 0;
    params.address_2 = 0;
    std::memcpy(output.data(), &params, sizeof(IoctlMapBufferEx));
    return 0;
}

u32 nvhost_vic::UnmapBufferEx(const std::vector<u8>& input, std::vector<u8>& output) {
    IoctlUnmapBufferEx params{};
    std::memcpy(&params, input.data(), sizeof(IoctlUnmapBufferEx));
    LOG_WARNING(Service_NVDRV, "(STUBBED) called");
    std::memcpy(output.data(), &params, sizeof(IoctlUnmapBufferEx));
    return 0;
}

u32 nvhost_vic::SubmitStub(const std::vector<u8>& input, std::vector<u8>& output) {
    IoctlSubmitStub params{};
    std::memcpy(&params, input.data(), sizeof(IoctlSubmitStub));
    LOG_WARNING(Service_NVDRV, "(STUBBED) called");

    // Workaround for Luigi's Mansion 3, as nvhost_vic is not implemented for asynch GPU
    params.command_buffer = {};

    std::memcpy(output.data(), &params, sizeof(IoctlSubmitStub));
    return 0;
}

u32 nvhost_vic::MapBufferStub(const std::vector<u8>& input, std::vector<u8>& output) {
    IoctlMapBuffer params{};
    std::memcpy(&params, input.data(), sizeof(IoctlMapBuffer));
    LOG_WARNING(Service_NVDRV, "(STUBBED) called with params={:08X}{:08X}", params.num_entries,
                params.data_address);
    params.num_entries = 0;
    params.data_address = 0;
    std::memcpy(output.data(), &params, sizeof(IoctlMapBuffer));
    return 0;
}

std::optional<nvhost_vic::BufferMap> nvhost_vic::FindBufferMap(GPUVAddr gpu_addr) const {
    auto it = std::find_if(buffer_mappings.begin(), buffer_mappings.upper_bound(gpu_addr),
                           [&](const std::pair<GPUVAddr, BufferMap> map) {
                               return (gpu_addr >= map.second.StartAddr() &&
                                       gpu_addr < map.second.EndAddr());
                           });

    ASSERT(it != buffer_mappings.end());
    return it->second;
}

void nvhost_vic::AddBufferMap(GPUVAddr gpu_addr, std::size_t size, VAddr cpu_addr,
                              bool is_allocated, u32 pin_id) {
    buffer_mappings.insert_or_assign(gpu_addr,
                                     BufferMap{gpu_addr, size, cpu_addr, is_allocated, pin_id});
}

std::optional<std::size_t> nvhost_vic::RemoveBufferMap(GPUVAddr gpu_addr) {
    const auto iter{buffer_mappings.find(gpu_addr)};
    if (iter == buffer_mappings.end()) {
        return std::nullopt;
    }
    std::size_t size{};
    if (iter->second.IsAllocated()) {
        size = iter->second.Size();
    }

    buffer_mappings.erase(iter);
    return size;
}

} // namespace Service::Nvidia::Devices
