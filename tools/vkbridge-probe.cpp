// SPDX-License-Identifier: MIT

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "vkbridge/device.hpp"
#include "vkbridge/fence.hpp"
#include "vkbridge/pattern.hpp"
#include "vkbridge/replay.hpp"
#include "vkbridge/sequence.hpp"
#include "vkbridge/vk_result.hpp"

namespace
{

constexpr VkDeviceSize kMebibyte = 1024 * 1024;
constexpr VkDeviceSize kCopyBytes = 512 * 1024;
constexpr VkDeviceSize kFillBytes = 999996;
constexpr uint32_t kOrderedCommands = 40;
constexpr int kCopyWarmupRuns = 5;
constexpr int kCopyTimedRuns = 50;
constexpr uint64_t kFenceTimeoutNs = 5'000'000'000ull;

struct Options {
    std::string device_substring;
    std::string screenshot_path = "vkbridge-probe.png";
    bool verbose = false;
    bool device_check = false;
    bool help = false;
};

void print_usage(std::ostream &out)
{
    out << "usage: vkbridge-probe [options]\n"
           "\n"
           "Runs the host-side Vulkan backend checks and prints one [ok]/[FAIL] line\n"
           "per check. Exit status is 0 only when every check passed.\n"
           "\n"
           "  --device-substring NAME  pick a physical device whose name contains NAME\n"
           "                           (default: $IOSVM_VK_DEVICE_SUBSTRING, else best device)\n"
           "  --screenshot PATH        PNG destination (default: vkbridge-probe.png)\n"
           "  --verbose                extra diagnostics on stderr\n"
           "  --device-check           exit 0 if a device can be opened; print nothing\n"
           "  --help                   show this text\n";
}

bool parse_options(int argc, char **argv, Options &options, std::string &error)
{
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        auto take_value = [&](std::string &target) -> bool {
            if (index + 1 >= argc) {
                error = argument + " needs a value";
                return false;
            }
            ++index;
            target = argv[index];
            return true;
        };
        if (argument == "--help" || argument == "-h") {
            options.help = true;
        } else if (argument == "--verbose") {
            options.verbose = true;
        } else if (argument == "--device-check") {
            options.device_check = true;
        } else if (argument == "--device-substring") {
            if (!take_value(options.device_substring)) {
                return false;
            }
        } else if (argument.rfind("--device-substring=", 0) == 0) {
            options.device_substring = argument.substr(19);
        } else if (argument == "--screenshot") {
            if (!take_value(options.screenshot_path)) {
                return false;
            }
        } else if (argument.rfind("--screenshot=", 0) == 0) {
            options.screenshot_path = argument.substr(13);
        } else {
            error = "unknown option: " + argument;
            return false;
        }
    }
    return true;
}

int fail(const std::string &check, const std::string &detail)
{
    std::cout << "[FAIL] " << check << ": " << detail << "\n";
    return 1;
}

std::string device_environment_substring(const Options &options)
{
    if (!options.device_substring.empty()) {
        return options.device_substring;
    }
    const char *environment = std::getenv("IOSVM_VK_DEVICE_SUBSTRING");
    return environment == nullptr ? std::string() : std::string(environment);
}

void print_device_info(const vkbridge::DeviceInfo &info)
{
    std::cerr << "[info] device type: " << vkbridge::vk_physical_device_type_name(info.type)
              << "\n";
    std::cerr << "[info] Vulkan API " << VK_API_VERSION_MAJOR(info.api_version) << "."
              << VK_API_VERSION_MINOR(info.api_version) << "."
              << VK_API_VERSION_PATCH(info.api_version) << ", driver 0x" << std::hex
              << info.driver_version << std::dec << "\n";
    std::cerr << "[info] queue family " << info.queue_family << " of " << info.queue_count
              << ", flags 0x" << std::hex << info.queue_flags << std::dec << "\n";
    std::cerr << "[info] maxMemoryAllocationSize " << info.max_buffer_bytes << " bytes\n";
}

int device_check(const Options &options)
{
    vkbridge::InstanceOptions instance_options;
    vkbridge::Result<vkbridge::Instance> instance = vkbridge::Instance::create(instance_options);
    if (!instance) {
        std::cerr << "device check failed: " << instance.status().to_string() << "\n";
        return 1;
    }
    vkbridge::DeviceOptions device_options;
    device_options.device_substring = device_environment_substring(options);
    vkbridge::Result<vkbridge::Device> device = vkbridge::Device::create(*instance, device_options);
    if (!device) {
        std::cerr << "device check failed: " << device.status().to_string() << "\n";
        return 1;
    }
    if (options.verbose) {
        std::cerr << "[info] device check found " << device->info().name << "\n";
    }
    return 0;
}

int run(const Options &options)
{
    vkbridge::InstanceOptions instance_options;
    vkbridge::Result<vkbridge::Instance> instance = vkbridge::Instance::create(instance_options);
    if (!instance) {
        return fail("device", instance.status().to_string());
    }

    vkbridge::DeviceOptions device_options;
    device_options.device_substring = device_environment_substring(options);
    vkbridge::Result<vkbridge::Device> device = vkbridge::Device::create(*instance, device_options);
    if (!device) {
        return fail("device", device.status().to_string());
    }
    std::cout << "[ok] device: " << device->info().name << "\n";
    if (options.verbose) {
        print_device_info(device->info());
    }

    vkbridge::Result<std::unique_ptr<vkbridge::Replay>> replay_result =
        vkbridge::Replay::create(*device);
    if (!replay_result) {
        return fail("create buffer 1 MiB", replay_result.status().to_string());
    }
    vkbridge::Replay &replay = **replay_result;

    const VkBufferUsageFlags storage_usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                                             VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                                             VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;

    vkbridge::Result<uint32_t> main_buffer = replay.create_buffer(kMebibyte, storage_usage);
    if (!main_buffer) {
        return fail("create buffer 1 MiB", main_buffer.status().to_string());
    }
    std::cout << "[ok] create buffer 1 MiB\n";

    const std::vector<uint8_t> one_mebibyte = vkbridge::make_upload_pattern(kMebibyte);
    vkbridge::Status status = replay.write(*main_buffer, one_mebibyte.data(), one_mebibyte.size());
    if (!status) {
        return fail("write 1 MiB, readback identical", status.to_string());
    }
    vkbridge::Result<std::vector<uint8_t>> written = replay.readback(*main_buffer, kMebibyte);
    if (!written) {
        return fail("write 1 MiB, readback identical", written.status().to_string());
    }
    if (written->size() != one_mebibyte.size() ||
        std::memcmp(written->data(), one_mebibyte.data(), one_mebibyte.size()) != 0) {
        return fail("write 1 MiB, readback identical",
                    "readback bytes differ from the upload pattern");
    }
    std::cout << "[ok] write 1 MiB, readback identical\n";
    if (options.verbose) {
        std::cerr << "[info] staging " << replay.staging().capacity() << " bytes, coherent="
                  << (replay.staging().coherent() ? "yes" : "no (flush/invalidate used)") << "\n";
    }

    vkbridge::Result<uint32_t> copy_source = replay.create_buffer(
        kCopyBytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    if (!copy_source) {
        return fail("blit copy 512 KiB",
                    "create source buffer: " + copy_source.status().to_string());
    }
    vkbridge::Result<uint32_t> copy_destination = replay.create_buffer(
        kCopyBytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    if (!copy_destination) {
        return fail("blit copy 512 KiB",
                    "create destination buffer: " + copy_destination.status().to_string());
    }

    const std::vector<uint8_t> copy_pattern = vkbridge::make_upload_pattern(kCopyBytes);
    const std::vector<uint8_t> other_pattern(static_cast<size_t>(kCopyBytes), 0xEEu);
    status = replay.write(*copy_source, copy_pattern.data(), copy_pattern.size());
    if (!status) {
        return fail("blit copy 512 KiB", "write source: " + status.to_string());
    }
    status = replay.write(*copy_destination, other_pattern.data(), other_pattern.size());
    if (!status) {
        return fail("blit copy 512 KiB", "write destination: " + status.to_string());
    }

    auto run_copy = [&]() -> vkbridge::Status {
        const vkbridge::Status copy_status =
            replay.blit_copy(*copy_source, *copy_destination, kCopyBytes);
        if (!copy_status) {
            return copy_status;
        }
        vkbridge::Result<uint64_t> sequence = replay.submit();
        if (!sequence) {
            return sequence.status();
        }
        return replay.wait_fence(*sequence, kFenceTimeoutNs);
    };

    for (int run_index = 0; run_index < kCopyWarmupRuns; ++run_index) {
        status = run_copy();
        if (!status) {
            return fail("blit copy 512 KiB", "warmup run failed: " + status.to_string());
        }
    }
    const std::chrono::steady_clock::time_point copy_start = std::chrono::steady_clock::now();
    for (int run_index = 0; run_index < kCopyTimedRuns; ++run_index) {
        status = run_copy();
        if (!status) {
            return fail("blit copy 512 KiB", "timed run failed: " + status.to_string());
        }
    }
    const std::chrono::steady_clock::time_point copy_end = std::chrono::steady_clock::now();
    const double average_ms =
        std::chrono::duration<double, std::milli>(copy_end - copy_start).count() /
        static_cast<double>(kCopyTimedRuns);

    vkbridge::Result<std::vector<uint8_t>> copied = replay.readback(*copy_destination, kCopyBytes);
    if (!copied) {
        return fail("blit copy 512 KiB", "readback: " + copied.status().to_string());
    }
    if (std::memcmp(copied->data(), copy_pattern.data(), static_cast<size_t>(kCopyBytes)) != 0) {
        return fail("blit copy 512 KiB", "destination differs from the source bytes");
    }
    std::cout << "[ok] blit copy 512 KiB (" << std::fixed << std::setprecision(2) << average_ms
              << " ms, n=" << kCopyTimedRuns << ")\n";

    vkbridge::Command fill_command;
    fill_command.type = vkbridge::CommandType::BlitFill;
    fill_command.handle = *main_buffer;
    fill_command.offset = 0;
    fill_command.size = kFillBytes;
    fill_command.value32 = vkbridge::kFillValue32;
    status = replay.apply(fill_command);
    if (!status) {
        return fail("blit fill 0..999996", status.to_string());
    }
    vkbridge::Result<std::vector<uint8_t>> filled = replay.readback(*main_buffer, kMebibyte);
    if (!filled) {
        return fail("blit fill 0..999996", "readback: " + filled.status().to_string());
    }
    if (!vkbridge::verify_fill(filled->data(), static_cast<size_t>(kFillBytes),
                               vkbridge::kFillValue32)) {
        return fail("blit fill 0..999996", "fill pattern mismatch");
    }
    if (std::memcmp(filled->data() + static_cast<size_t>(kFillBytes),
                    one_mebibyte.data() + static_cast<size_t>(kFillBytes),
                    static_cast<size_t>(kMebibyte - kFillBytes)) != 0) {
        return fail("blit fill 0..999996", "bytes beyond the fill range changed");
    }
    std::cout << "[ok] blit fill 0..999996\n";

    const VkDeviceSize ordered_bytes = static_cast<VkDeviceSize>(kOrderedCommands) * 4;
    vkbridge::Result<uint32_t> ordered = replay.create_buffer(ordered_bytes, storage_usage);
    if (!ordered) {
        return fail("40 command buffers in order",
                    "create ordered buffer: " + ordered.status().to_string());
    }
    vkbridge::Result<uint32_t> counter = replay.create_buffer(4, storage_usage);
    if (!counter) {
        return fail("40 command buffers in order",
                    "create counter buffer: " + counter.status().to_string());
    }
    const std::vector<uint8_t> zeros(static_cast<size_t>(ordered_bytes), 0u);
    status = replay.write(*ordered, zeros.data(), zeros.size());
    if (!status) {
        return fail("40 command buffers in order", "clear ordered buffer: " + status.to_string());
    }
    const std::vector<uint8_t> zero_word(4, 0u);
    status = replay.write(*counter, zero_word.data(), zero_word.size());
    if (!status) {
        return fail("40 command buffers in order", "clear counter buffer: " + status.to_string());
    }

    std::vector<uint64_t> batches;
    for (uint32_t index = 0; index < kOrderedCommands; ++index) {
        vkbridge::Command ordered_fill;
        ordered_fill.type = vkbridge::CommandType::BlitFill;
        ordered_fill.handle = *ordered;
        ordered_fill.offset = static_cast<uint64_t>(index) * 4;
        ordered_fill.size = 4;
        ordered_fill.value32 = 0xA0000000u + index;
        status = replay.apply(ordered_fill);
        if (!status) {
            return fail("40 command buffers in order",
                        "record batch " + std::to_string(index) + ": " + status.to_string());
        }
        vkbridge::Command counter_fill;
        counter_fill.type = vkbridge::CommandType::BlitFill;
        counter_fill.handle = *counter;
        counter_fill.offset = 0;
        counter_fill.size = 4;
        counter_fill.value32 = index + 1;
        status = replay.apply(counter_fill);
        if (!status) {
            return fail("40 command buffers in order",
                        "record counter " + std::to_string(index) + ": " + status.to_string());
        }
        vkbridge::Result<uint64_t> sequence = replay.submit();
        if (!sequence) {
            return fail("40 command buffers in order", "submit batch " + std::to_string(index) +
                                                           ": " + sequence.status().to_string());
        }
        batches.push_back(*sequence);
    }

    vkbridge::CompletionOrder completion;
    for (uint64_t sequence : batches) {
        status = replay.wait_fence(sequence, kFenceTimeoutNs);
        if (!status) {
            return fail("40 command buffers in order", "wait for sequence " +
                                                           std::to_string(sequence) + ": " +
                                                           status.to_string());
        }
        completion.observe(sequence);
    }
    bool monotonic = true;
    for (size_t index = 1; index < batches.size(); ++index) {
        if (batches[index] <= batches[index - 1]) {
            monotonic = false;
        }
    }

    vkbridge::Result<std::vector<uint8_t>> ordered_data = replay.readback(*ordered, ordered_bytes);
    if (!ordered_data) {
        return fail("40 command buffers in order",
                    "readback ordered buffer: " + ordered_data.status().to_string());
    }
    vkbridge::Result<std::vector<uint8_t>> counter_data = replay.readback(*counter, 4);
    if (!counter_data) {
        return fail("40 command buffers in order",
                    "readback counter: " + counter_data.status().to_string());
    }
    for (uint32_t index = 0; index < kOrderedCommands; ++index) {
        const uint32_t expected = 0xA0000000u + index;
        if (vkbridge::read_le32(ordered_data->data() + index * 4) != expected) {
            return fail("40 command buffers in order",
                        "word " + std::to_string(index) + " was not filled by its command buffer");
        }
    }
    if (vkbridge::read_le32(counter_data->data()) != kOrderedCommands) {
        return fail("40 command buffers in order",
                    "counter ended at a value other than 40, so execution order was not preserved");
    }
    if (!monotonic || completion.inverted()) {
        return fail("40 command buffers in order",
                    "completion order did not match submission order");
    }
    if (options.verbose) {
        std::cerr << "[info] submitted " << replay.submitted_batches()
                  << " batches for the ordering check\n";
    }
    std::cout << "[ok] 40 command buffers in order\n";

    vkbridge::Result<uint32_t> fence_buffer = replay.create_buffer(
        8, VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    if (!fence_buffer) {
        return fail("fence signal/wait ordering",
                    "create fence buffer: " + fence_buffer.status().to_string());
    }
    status = replay.blit_fill(*fence_buffer, 0, 4, 0xB0000001u);
    if (!status) {
        return fail("fence signal/wait ordering", "record batch A: " + status.to_string());
    }
    vkbridge::Result<uint64_t> sequence_a = replay.submit();
    if (!sequence_a) {
        return fail("fence signal/wait ordering",
                    "submit batch A: " + sequence_a.status().to_string());
    }
    status = replay.blit_fill(*fence_buffer, 4, 4, 0xB0000002u);
    if (!status) {
        return fail("fence signal/wait ordering", "record batch B: " + status.to_string());
    }
    vkbridge::Result<uint64_t> sequence_b = replay.submit();
    if (!sequence_b) {
        return fail("fence signal/wait ordering",
                    "submit batch B: " + sequence_b.status().to_string());
    }
    if (*sequence_b != *sequence_a + 1) {
        return fail("fence signal/wait ordering", "submission sequence is not monotonic (" +
                                                      std::to_string(*sequence_a) + " then " +
                                                      std::to_string(*sequence_b) + ")");
    }
    status = replay.wait_fence(*sequence_b, kFenceTimeoutNs);
    if (!status) {
        return fail("fence signal/wait ordering",
                    "later fence did not signal: " + status.to_string());
    }
    vkbridge::Result<bool> earlier_signaled = replay.fence_signaled(*sequence_a);
    if (!earlier_signaled || !earlier_signaled.value()) {
        return fail("fence signal/wait ordering",
                    "earlier batch was still pending after the later one completed");
    }
    status = replay.wait_fence(*sequence_a, 0);
    if (!status) {
        return fail("fence signal/wait ordering",
                    "immediate wait on the earlier fence failed: " + status.to_string());
    }

    vkbridge::Result<vkbridge::Fence> timeout_fence = vkbridge::Fence::create(device->handle(), 0);
    if (!timeout_fence) {
        return fail("fence signal/wait ordering",
                    "create timeout fence: " + timeout_fence.status().to_string());
    }
    const vkbridge::Status timeout_status = timeout_fence->wait(1'000'000);
    if (timeout_status.code() != vkbridge::Error::Timeout) {
        return fail("fence signal/wait ordering",
                    "waiting for an unsignaled fence did not report a timeout");
    }

    vkbridge::Result<std::vector<uint8_t>> fence_data = replay.readback(*fence_buffer, 8);
    if (!fence_data) {
        return fail("fence signal/wait ordering", "readback: " + fence_data.status().to_string());
    }
    if (vkbridge::read_le32(fence_data->data()) != 0xB0000001u ||
        vkbridge::read_le32(fence_data->data() + 4) != 0xB0000002u) {
        return fail("fence signal/wait ordering", "fence batches did not reach the buffer");
    }
    vkbridge::CompletionOrder fence_order;
    fence_order.observe(*sequence_a);
    fence_order.observe(*sequence_b);
    if (fence_order.inverted()) {
        return fail("fence signal/wait ordering", "completion order is inverted");
    }
    std::cout << "[ok] fence signal/wait ordering\n";

    vkbridge::Result<vkbridge::ScreenshotInfo> screenshot =
        replay.screenshot(options.screenshot_path);
    if (!screenshot) {
        return fail("screenshot " + options.screenshot_path, screenshot.status().to_string());
    }
    const vkbridge::ScreenshotLayout layout =
        vkbridge::make_screenshot_layout(screenshot->width, screenshot->height);
    const std::vector<uint8_t> reference = vkbridge::render_layout(layout);
    if (reference.size() != screenshot->rgba.size()) {
        return fail("screenshot " + options.screenshot_path, "GPU frame has an unexpected size");
    }
    if (reference != screenshot->rgba) {
        return fail("screenshot " + options.screenshot_path,
                    "GPU frame differs from the CPU layout model");
    }
    const size_t reference_unique =
        vkbridge::count_unique_colours(reference.data(), reference.size() / 4);
    if (screenshot->unique_colours != reference_unique) {
        return fail("screenshot " + options.screenshot_path,
                    "unique colour count does not match the CPU layout model");
    }

    std::FILE *file = std::fopen(options.screenshot_path.c_str(), "rb");
    if (file == nullptr) {
        return fail("screenshot " + options.screenshot_path,
                    "the PNG cannot be opened after writing it");
    }
    uint8_t signature[8] = {};
    const size_t signature_size = std::fread(signature, 1, sizeof(signature), file);
    std::fclose(file);
    static const uint8_t kPngSignature[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    if (signature_size != sizeof(signature) ||
        std::memcmp(signature, kPngSignature, sizeof(signature)) != 0) {
        return fail("screenshot " + options.screenshot_path, "the written file is not a PNG");
    }

    std::cout << "[ok] screenshot " << options.screenshot_path << " (" << screenshot->width << "x"
              << screenshot->height << ", " << screenshot->unique_colours << " unique colours)\n";
    return 0;
}

} // namespace

int main(int argc, char **argv)
{
    Options options;
    std::string error;
    if (!parse_options(argc, argv, options, error)) {
        std::cerr << "vkbridge-probe: " << error << "\n";
        print_usage(std::cerr);
        return 2;
    }
    if (options.help) {
        print_usage(std::cout);
        return 0;
    }
    if (options.device_check) {
        return device_check(options);
    }
    return run(options);
}
