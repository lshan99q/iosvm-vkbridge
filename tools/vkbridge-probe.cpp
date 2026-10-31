// SPDX-License-Identifier: MIT

#include <sys/stat.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <unistd.h>

#include "vkbridge/device.hpp"
#include "vkbridge/fence.hpp"
#include "vkbridge/object_table.hpp"
#include "vkbridge/oracle.hpp"
#include "vkbridge/pattern.hpp"
#include "vkbridge/png.hpp"
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
constexpr std::chrono::seconds kOrderingTimeout{10};

// Sentinels written through the normal staging path before an attested operation. If the
// compute shader does not run, the host reads these words back unchanged.
constexpr uint32_t kAttestationSentinel0 = 0x5EEDC0DEu;
constexpr uint32_t kAttestationSentinel1 = 0xBAD0F00Du;

constexpr uint32_t kCopyAttestationNonce = 0xC0FFEE01u;
constexpr uint32_t kFillAttestationNonce = 0xC0FFEE02u;
constexpr uint32_t kScreenshotAttestationNonce = 0xC0FFEE03u;

enum ExitCode {
    kExitFailure = 1,
    kExitUsage = 2,
    kExitNoDevice = 3,
    kExitIcdMisconfigured = 4,
};

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
           "  --device-check           exit 0 if a device can be opened; prints a reason\n"
           "                           on stderr for every other outcome\n"
           "  --help                   show this text\n"
           "\n"
           "Exit codes:\n"
           "  0  every check passed (or --device-check opened a device)\n"
           "  1  a check failed\n"
           "  2  bad command line\n"
           "  3  no Vulkan device is present (--device-check prints [skip])\n"
           "  4  the ICD named by $VK_DRIVER_FILES / $VK_ICD_FILENAMES is missing, unreadable\n"
           "     or failed to load: a misconfiguration, not a skip\n";
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
    return kExitFailure;
}

std::string format_hex32(uint32_t value)
{
    std::ostringstream stream;
    stream << "0x" << std::hex << std::setw(8) << std::setfill('0') << value;
    return stream.str();
}

// ---------------------------------------------------------------------------
// ICD environment inspection
// ---------------------------------------------------------------------------

enum class IcdState {
    Unset,
    Explicit,
    Misconfigured,
};

struct IcdEnvironment {
    IcdState state = IcdState::Unset;
    std::string variable;
    std::string detail;
};

// The Vulkan loader prefers VK_DRIVER_FILES; VK_ICD_FILENAMES is its deprecated alias.
// A variable that names a missing or unreadable file is a misconfiguration, not a machine
// without a GPU, so it must never look like a skipped-but-passed run.
IcdEnvironment inspect_icd_environment()
{
    const char *const names[] = {"VK_DRIVER_FILES", "VK_ICD_FILENAMES"};
    for (const char *name : names) {
        const char *value = std::getenv(name);
        if (value == nullptr || *value == '\0') {
            continue;
        }
        IcdEnvironment environment;
        environment.variable = name;
        environment.detail = value;
        const std::string list = value;
        size_t start = 0;
        while (true) {
            size_t end = list.find(':', start);
            if (end == std::string::npos) {
                end = list.size();
            }
            const std::string entry = list.substr(start, end - start);
            if (entry.empty()) {
                environment.state = IcdState::Misconfigured;
                environment.detail = std::string(name) + " contains an empty ICD path";
                return environment;
            }
            struct stat info {
            };
            if (::stat(entry.c_str(), &info) != 0) {
                environment.state = IcdState::Misconfigured;
                environment.detail =
                    std::string(name) + " names " + entry + ": " + std::strerror(errno);
                return environment;
            }
            if (!S_ISREG(info.st_mode)) {
                environment.state = IcdState::Misconfigured;
                environment.detail =
                    std::string(name) + " names " + entry + ", which is not a regular file";
                return environment;
            }
            if (::access(entry.c_str(), R_OK) != 0) {
                environment.state = IcdState::Misconfigured;
                environment.detail =
                    std::string(name) + " names " + entry + ", which is not readable";
                return environment;
            }
            if (end == list.size()) {
                break;
            }
            start = end + 1;
        }
        environment.state = IcdState::Explicit;
        return environment;
    }
    return IcdEnvironment{};
}

void report_icd_environment(const Options &options, const IcdEnvironment &environment)
{
    if (environment.state == IcdState::Unset || !options.verbose) {
        return;
    }
    std::cerr << "[info] using " << environment.variable << "=" << environment.detail << "\n";
    if (environment.variable == "VK_ICD_FILENAMES") {
        std::cerr << "[info] VK_ICD_FILENAMES is deprecated; prefer VK_DRIVER_FILES\n";
    }
}

// ---------------------------------------------------------------------------
// Memory reporting and assertions
// ---------------------------------------------------------------------------

std::string format_memory_flags(const vkbridge::MemoryTypeInfo &info)
{
    std::string text;
    const auto append = [&text](const char *name) {
        if (!text.empty()) {
            text += "|";
        }
        text += name;
    };
    if (info.device_local) {
        append("DEVICE_LOCAL");
    }
    if (info.host_visible) {
        append("HOST_VISIBLE");
    }
    if (info.host_coherent) {
        append("HOST_COHERENT");
    }
    if (info.host_cached) {
        append("HOST_CACHED");
    }
    if ((info.heap_flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) != 0) {
        append("HEAP_DEVICE_LOCAL");
    }
    return text.empty() ? "NONE" : text;
}

void print_memory_types(const vkbridge::Device &device)
{
    const VkPhysicalDeviceMemoryProperties &properties = device.memory_properties();
    std::cerr << "[info] memory heaps: " << properties.memoryHeapCount
              << ", memory types: " << properties.memoryTypeCount << "\n";
    for (uint32_t index = 0; index < properties.memoryTypeCount; ++index) {
        vkbridge::MemoryTypeInfo info;
        if (!vkbridge::memory_type_info(device, index, info)) {
            continue;
        }
        std::cerr << "[info] memory type " << index << ": heap " << info.heap_index
                  << ", heap size " << info.heap_size << " bytes, flags 0x" << std::hex
                  << std::setw(8) << std::setfill('0') << info.flags << std::dec << " ["
                  << format_memory_flags(info) << "]\n";
    }
}

// True when the device exposes a device-local memory type the host cannot map. On such a
// device a blit buffer that is host-visible is a genuine implementation fault. On a UMA
// driver (lavapipe has exactly one memory type, DEVICE_LOCAL|HOST_VISIBLE|...) the
// property simply does not exist and the device-side oracle carries the proof instead.
bool has_segregated_device_local_memory(const vkbridge::Device &device)
{
    const VkPhysicalDeviceMemoryProperties &properties = device.memory_properties();
    for (uint32_t index = 0; index < properties.memoryTypeCount; ++index) {
        const VkMemoryPropertyFlags flags = properties.memoryTypes[index].propertyFlags;
        if ((flags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != 0 &&
            (flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) == 0) {
            return true;
        }
    }
    return false;
}

void print_object_memory(const vkbridge::Device &device, const vkbridge::Replay &replay,
                         uint32_t handle, const char *role)
{
    const vkbridge::ObjectRecord *record = replay.objects().find(handle);
    if (record == nullptr) {
        return;
    }
    vkbridge::MemoryTypeInfo info;
    if (!vkbridge::memory_type_info(device, record->memory_type_index, info)) {
        return;
    }
    std::cerr << "[info] " << role << " (handle " << handle << "): memory type "
              << record->memory_type_index << ", heap " << info.heap_index << ", heap size "
              << info.heap_size << " bytes, flags 0x" << std::hex << std::setw(8)
              << std::setfill('0') << info.flags << std::dec << " [" << format_memory_flags(info)
              << "]\n";
}

// Empty on success, otherwise a failure detail. A buffer whose contents the probe treats
// as device-produced must live in device-local memory; when the device has a non-mappable
// device-local type it must not be host-visible either.
std::string check_device_local_object(const vkbridge::Replay &replay, uint32_t handle,
                                      const char *role, bool segregated_device_local)
{
    const vkbridge::ObjectRecord *record = replay.objects().find(handle);
    if (record == nullptr) {
        return std::string(role) + " is missing from the object table";
    }
    if (!record->device_local) {
        return std::string(role) + " is not device-local (memory type " +
               std::to_string(record->memory_type_index) +
               "), so a host write could produce its contents without the device";
    }
    if (segregated_device_local && record->host_visible) {
        return std::string(role) + " is host-visible (memory type " +
               std::to_string(record->memory_type_index) +
               ") even though this device exposes a non-mappable device-local memory type";
    }
    return {};
}

std::string check_staging_is_host_visible(const vkbridge::Device &device,
                                          const vkbridge::Replay &replay)
{
    vkbridge::MemoryTypeInfo info;
    if (!vkbridge::memory_type_info(device, replay.staging().memory_type_index(), info)) {
        return "the staging buffer has no memory type";
    }
    if (!info.host_visible || replay.staging().host_pointer() == nullptr) {
        return "the readback staging buffer is not host-visible/mapped, so host readback and "
               "the byte comparison would be impossible";
    }
    return {};
}

// ---------------------------------------------------------------------------
// Attestation helpers
// ---------------------------------------------------------------------------

vkbridge::Result<uint32_t> create_attestation_buffer(vkbridge::Replay &replay)
{
    return replay.create_buffer(8, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                       VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
}

vkbridge::Status write_attestation_sentinel(vkbridge::Replay &replay, uint32_t handle)
{
    const uint32_t words[2] = {kAttestationSentinel0, kAttestationSentinel1};
    return replay.write(handle, words, sizeof(words));
}

vkbridge::Result<vkbridge::Attestation> read_attestation(vkbridge::Replay &replay, uint32_t handle)
{
    vkbridge::Result<std::vector<uint8_t>> bytes = replay.readback(handle, 8);
    if (!bytes) {
        return vkbridge::err_result<vkbridge::Attestation>(bytes.status());
    }
    vkbridge::Attestation attestation;
    attestation.checksum = vkbridge::read_le32(bytes->data());
    attestation.fingerprint = vkbridge::read_le32(bytes->data() + 4);
    return vkbridge::ok_result(attestation);
}

// Empty when the shader-produced words match the host's own expectation.
std::string verify_attestation(const vkbridge::Attestation &actual, const uint8_t *expected_bytes,
                               size_t expected_size, uint32_t nonce, const char *role)
{
    if (actual.checksum == kAttestationSentinel0 && actual.fingerprint == kAttestationSentinel1) {
        return std::string(role) + ": the compute shader did not write the attestation buffer "
                                   "(the sentinel is unchanged), so the operation did not run "
                                   "through the recorded device commands";
    }
    if (actual.checksum == 0 && actual.fingerprint == 0) {
        return std::string(role) + ": the attestation buffer is all zero, so no compute shader "
                                   "executed as part of this operation";
    }
    const uint32_t expected_checksum =
        vkbridge::attestation_checksum(expected_bytes, expected_size, nonce);
    const uint32_t expected_fingerprint = vkbridge::kAttestationFingerprint ^ nonce;
    if (actual.checksum != expected_checksum) {
        return std::string(role) + ": the device-side checksum is " +
               format_hex32(actual.checksum) + ", the host expects " +
               format_hex32(expected_checksum);
    }
    if (actual.fingerprint != expected_fingerprint) {
        return std::string(role) + ": the device-side fingerprint is " +
               format_hex32(actual.fingerprint) + ", the host expects " +
               format_hex32(expected_fingerprint);
    }
    return {};
}

std::vector<uint8_t> make_fill_pattern(VkDeviceSize bytes, uint32_t value32)
{
    std::vector<uint8_t> pattern(static_cast<size_t>(bytes));
    for (size_t word = 0; word < pattern.size() / 4; ++word) {
        vkbridge::write_le32(pattern.data() + word * 4, value32);
    }
    return pattern;
}

// ---------------------------------------------------------------------------
// Device setup
// ---------------------------------------------------------------------------

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
    std::cerr << "[info] timestamps on this queue: " << info.timestamp_valid_bits
              << " valid bits, period " << info.timestamp_period_ns << " ns/tick\n";
}

int device_check(const Options &options)
{
    const IcdEnvironment environment = inspect_icd_environment();
    if (environment.state == IcdState::Misconfigured) {
        std::cerr << "[error] " << environment.detail
                  << "\n        (the Vulkan ICD environment is misconfigured, not absent)\n";
        return kExitIcdMisconfigured;
    }
    report_icd_environment(options, environment);

    vkbridge::InstanceOptions instance_options;
    vkbridge::Result<vkbridge::Instance> instance = vkbridge::Instance::create(instance_options);
    if (!instance) {
        if (environment.state == IcdState::Explicit) {
            std::cerr << "[error] the ICD named by " << environment.variable
                      << " failed to load: " << instance.status().to_string() << "\n";
            return kExitIcdMisconfigured;
        }
        std::cerr << "[skip] no Vulkan device: " << instance.status().to_string() << "\n";
        return kExitNoDevice;
    }
    vkbridge::DeviceOptions device_options;
    device_options.device_substring = device_environment_substring(options);
    vkbridge::Result<vkbridge::Device> device = vkbridge::Device::create(*instance, device_options);
    if (!device) {
        if (device.status().code() == vkbridge::Error::NoDevice) {
            if (environment.state == IcdState::Explicit) {
                std::cerr << "[error] the ICD named by " << environment.variable
                          << " loaded but exposed no usable device: " << device.status().to_string()
                          << "\n";
                return kExitIcdMisconfigured;
            }
            std::cerr << "[skip] no Vulkan device: " << device.status().to_string() << "\n";
            return kExitNoDevice;
        }
        std::cerr << "[error] Vulkan device creation failed: " << device.status().to_string()
                  << "\n";
        return kExitFailure;
    }
    if (options.verbose) {
        std::cerr << "[info] device check found " << device->info().name << "\n";
    }
    return 0;
}

// ---------------------------------------------------------------------------
// The probe itself
// ---------------------------------------------------------------------------

int run(const Options &options)
{
    const IcdEnvironment environment = inspect_icd_environment();
    if (environment.state == IcdState::Misconfigured) {
        std::cerr << "[error] " << environment.detail
                  << "\n        (the Vulkan ICD environment is misconfigured, not absent)\n";
        return kExitIcdMisconfigured;
    }
    report_icd_environment(options, environment);

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
    const bool segregated_device_local = has_segregated_device_local_memory(*device);
    if (options.verbose) {
        print_device_info(device->info());
        print_memory_types(*device);
    }
    if (!segregated_device_local) {
        std::cerr << "[info] this device has no device-local memory type that is not host-visible "
                     "(UMA): the not-mappable assertion cannot be made, and the device-side "
                     "oracle carries the mechanism proof\n";
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
    std::string memory_problem = check_staging_is_host_visible(*device, replay);
    if (!memory_problem.empty()) {
        return fail("write 1 MiB, readback identical", memory_problem);
    }
    memory_problem = check_device_local_object(replay, *main_buffer, "the uploaded buffer",
                                               segregated_device_local);
    if (!memory_problem.empty()) {
        return fail("write 1 MiB, readback identical", memory_problem);
    }
    if (options.verbose) {
        print_object_memory(*device, replay, *main_buffer, "1 MiB buffer");
        std::cerr << "[info] staging memory: type " << replay.staging().memory_type_index()
                  << ", capacity " << replay.staging().capacity() << " bytes, coherent="
                  << (replay.staging().coherent() ? "yes" : "no (flush/invalidate used)") << "\n";
    }
    std::cout << "[ok] write 1 MiB, readback identical\n";

    // ------------------------------------------------------------------
    // blit copy: timed with device timestamps, attested by a compute shader
    // ------------------------------------------------------------------
    const std::string copy_check = "blit copy 512 KiB";

    vkbridge::Result<uint32_t> copy_source = replay.create_buffer(
        kCopyBytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    if (!copy_source) {
        return fail(copy_check, "create source buffer: " + copy_source.status().to_string());
    }
    vkbridge::Result<uint32_t> copy_destination = replay.create_buffer(
        kCopyBytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    if (!copy_destination) {
        return fail(copy_check,
                    "create destination buffer: " + copy_destination.status().to_string());
    }
    vkbridge::Result<uint32_t> copy_attestation = create_attestation_buffer(replay);
    if (!copy_attestation) {
        return fail(copy_check,
                    "create attestation buffer: " + copy_attestation.status().to_string());
    }

    memory_problem =
        check_device_local_object(replay, *copy_source, "the copy source", segregated_device_local);
    if (!memory_problem.empty()) {
        return fail(copy_check, memory_problem);
    }
    memory_problem = check_device_local_object(replay, *copy_destination, "the copy destination",
                                               segregated_device_local);
    if (!memory_problem.empty()) {
        return fail(copy_check, memory_problem);
    }
    memory_problem = check_device_local_object(replay, *copy_attestation, "the attestation buffer",
                                               segregated_device_local);
    if (!memory_problem.empty()) {
        return fail(copy_check, memory_problem);
    }
    if (options.verbose) {
        print_object_memory(*device, replay, *copy_source, "copy source");
        print_object_memory(*device, replay, *copy_destination, "copy destination");
        print_object_memory(*device, replay, *copy_attestation, "copy attestation");
    }

    status = write_attestation_sentinel(replay, *copy_attestation);
    if (!status) {
        return fail(copy_check, "seed the attestation buffer: " + status.to_string());
    }
    const std::vector<uint8_t> copy_pattern = vkbridge::make_upload_pattern(kCopyBytes);
    const std::vector<uint8_t> other_pattern(static_cast<size_t>(kCopyBytes), 0xEEu);
    status = replay.write(*copy_source, copy_pattern.data(), copy_pattern.size());
    if (!status) {
        return fail(copy_check, "write source: " + status.to_string());
    }
    status = replay.write(*copy_destination, other_pattern.data(), other_pattern.size());
    if (!status) {
        return fail(copy_check, "write destination: " + status.to_string());
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
            return fail(copy_check, "warmup run failed: " + status.to_string());
        }
    }

    double submit_fence_ms = 0.0;
    uint64_t device_ns = 0;
    int device_samples = 0;
    for (int run_index = 0; run_index < kCopyTimedRuns; ++run_index) {
        bool use_device_time = replay.device_timestamps_supported();
        uint32_t begin_token = 0;
        uint32_t end_token = 0;
        if (use_device_time) {
            const vkbridge::Result<uint32_t> token = replay.record_timestamp();
            if (token) {
                begin_token = *token;
            } else {
                use_device_time = false;
            }
        }
        status = replay.blit_copy(*copy_source, *copy_destination, kCopyBytes);
        if (!status) {
            return fail(copy_check, "timed run failed: " + status.to_string());
        }
        if (use_device_time) {
            const vkbridge::Result<uint32_t> token = replay.record_timestamp();
            if (token) {
                end_token = *token;
            } else {
                use_device_time = false;
            }
        }
        const std::chrono::steady_clock::time_point wall_begin = std::chrono::steady_clock::now();
        vkbridge::Result<uint64_t> sequence = replay.submit();
        if (!sequence) {
            return fail(copy_check, "timed run failed: " + sequence.status().to_string());
        }
        status = replay.wait_fence(*sequence, kFenceTimeoutNs);
        if (!status) {
            return fail(copy_check, "timed run failed: " + status.to_string());
        }
        submit_fence_ms +=
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - wall_begin)
                .count();
        if (use_device_time) {
            const vkbridge::Result<uint64_t> begin_ns = replay.timestamp_ns(begin_token);
            const vkbridge::Result<uint64_t> end_ns = replay.timestamp_ns(end_token);
            if (begin_ns && end_ns && *end_ns >= *begin_ns) {
                device_ns += *end_ns - *begin_ns;
                ++device_samples;
            }
        }
    }
    submit_fence_ms /= static_cast<double>(kCopyTimedRuns);

    // The attested copy records vkCmdCopyBuffer and the oracle dispatch into one command
    // buffer: a host-side implementation of the copy never writes the attestation buffer.
    status = replay.blit_copy(*copy_source, *copy_destination, kCopyBytes, 0, 0, *copy_attestation,
                              kCopyAttestationNonce);
    if (!status) {
        return fail(copy_check, "record attested copy: " + status.to_string());
    }
    vkbridge::Result<uint64_t> attested_sequence = replay.submit();
    if (!attested_sequence) {
        return fail(copy_check, "submit attested copy: " + attested_sequence.status().to_string());
    }
    status = replay.wait_fence(*attested_sequence, kFenceTimeoutNs);
    if (!status) {
        return fail(copy_check, "wait for attested copy: " + status.to_string());
    }
    vkbridge::Result<vkbridge::Attestation> copied_attestation =
        read_attestation(replay, *copy_attestation);
    if (!copied_attestation) {
        return fail(copy_check, "read attestation: " + copied_attestation.status().to_string());
    }
    memory_problem = verify_attestation(*copied_attestation, copy_pattern.data(),
                                        copy_pattern.size(), kCopyAttestationNonce, "blit copy");
    if (!memory_problem.empty()) {
        return fail(copy_check, memory_problem);
    }

    vkbridge::Result<std::vector<uint8_t>> copied = replay.readback(*copy_destination, kCopyBytes);
    if (!copied) {
        return fail(copy_check, "readback: " + copied.status().to_string());
    }
    if (std::memcmp(copied->data(), copy_pattern.data(), static_cast<size_t>(kCopyBytes)) != 0) {
        return fail(copy_check, "destination differs from the source bytes");
    }

    std::cout << "[ok] blit copy 512 KiB (";
    if (device_samples == kCopyTimedRuns) {
        const double device_ms = static_cast<double>(device_ns) / 1.0e6 / device_samples;
        std::cout << "device " << std::fixed << std::setprecision(3) << device_ms << " ms, ";
    }
    std::cout << "submit+fence " << std::fixed << std::setprecision(2) << submit_fence_ms
              << " ms, n=" << kCopyTimedRuns << ", device checksum ok";
    if (device_samples != kCopyTimedRuns) {
        std::cout << ", no device timestamps";
    }
    std::cout << ")\n";

    // ------------------------------------------------------------------
    // blit fill: attested by the same compute-shader oracle
    // ------------------------------------------------------------------
    const std::string fill_check = "blit fill 0..999996";
    vkbridge::Command fill_command;
    fill_command.type = vkbridge::CommandType::BlitFill;
    fill_command.handle = *main_buffer;
    fill_command.offset = 0;
    fill_command.size = kFillBytes;
    fill_command.value32 = vkbridge::kFillValue32;
    status = replay.apply(fill_command);
    if (!status) {
        return fail(fill_check, status.to_string());
    }
    vkbridge::Result<uint64_t> fill_sequence = replay.submit();
    if (!fill_sequence) {
        return fail(fill_check, "submit fill: " + fill_sequence.status().to_string());
    }
    status = replay.wait_fence(*fill_sequence, kFenceTimeoutNs);
    if (!status) {
        return fail(fill_check, "wait for fill: " + status.to_string());
    }

    memory_problem = check_device_local_object(replay, *main_buffer, "the filled buffer",
                                               segregated_device_local);
    if (!memory_problem.empty()) {
        return fail(fill_check, memory_problem);
    }
    if (options.verbose) {
        print_object_memory(*device, replay, *main_buffer, "filled buffer");
    }

    vkbridge::Result<uint32_t> fill_attestation = create_attestation_buffer(replay);
    if (!fill_attestation) {
        return fail(fill_check,
                    "create attestation buffer: " + fill_attestation.status().to_string());
    }
    status = write_attestation_sentinel(replay, *fill_attestation);
    if (!status) {
        return fail(fill_check, "seed the attestation buffer: " + status.to_string());
    }
    status = replay.blit_fill(*main_buffer, 0, kFillBytes, vkbridge::kFillValue32,
                              *fill_attestation, kFillAttestationNonce);
    if (!status) {
        return fail(fill_check, "record attested fill: " + status.to_string());
    }
    vkbridge::Result<uint64_t> attested_fill = replay.submit();
    if (!attested_fill) {
        return fail(fill_check, "submit attested fill: " + attested_fill.status().to_string());
    }
    status = replay.wait_fence(*attested_fill, kFenceTimeoutNs);
    if (!status) {
        return fail(fill_check, "wait for attested fill: " + status.to_string());
    }
    const std::vector<uint8_t> fill_pattern = make_fill_pattern(kFillBytes, vkbridge::kFillValue32);
    vkbridge::Result<vkbridge::Attestation> filled_attestation =
        read_attestation(replay, *fill_attestation);
    if (!filled_attestation) {
        return fail(fill_check, "read attestation: " + filled_attestation.status().to_string());
    }
    memory_problem = verify_attestation(*filled_attestation, fill_pattern.data(),
                                        fill_pattern.size(), kFillAttestationNonce, "blit fill");
    if (!memory_problem.empty()) {
        return fail(fill_check, memory_problem);
    }

    vkbridge::Result<std::vector<uint8_t>> filled = replay.readback(*main_buffer, kMebibyte);
    if (!filled) {
        return fail(fill_check, "readback: " + filled.status().to_string());
    }
    if (!vkbridge::verify_fill(filled->data(), static_cast<size_t>(kFillBytes),
                               vkbridge::kFillValue32)) {
        return fail(fill_check, "fill pattern mismatch");
    }
    if (std::memcmp(filled->data() + static_cast<size_t>(kFillBytes),
                    one_mebibyte.data() + static_cast<size_t>(kFillBytes),
                    static_cast<size_t>(kMebibyte - kFillBytes)) != 0) {
        return fail(fill_check, "bytes beyond the fill range changed");
    }
    std::cout << "[ok] blit fill 0..999996 (device checksum ok)\n";

    // ------------------------------------------------------------------
    // 40 separate command buffers, actual completion order observed by polling
    // ------------------------------------------------------------------
    const std::string order_check = "40 command buffers in order";
    const VkDeviceSize ordered_bytes = static_cast<VkDeviceSize>(kOrderedCommands) * 4;
    vkbridge::Result<uint32_t> ordered = replay.create_buffer(ordered_bytes, storage_usage);
    if (!ordered) {
        return fail(order_check, "create ordered buffer: " + ordered.status().to_string());
    }
    vkbridge::Result<uint32_t> counter = replay.create_buffer(4, storage_usage);
    if (!counter) {
        return fail(order_check, "create counter buffer: " + counter.status().to_string());
    }
    const std::vector<uint8_t> zeros(static_cast<size_t>(ordered_bytes), 0u);
    status = replay.write(*ordered, zeros.data(), zeros.size());
    if (!status) {
        return fail(order_check, "clear ordered buffer: " + status.to_string());
    }
    const std::vector<uint8_t> zero_word(4, 0u);
    status = replay.write(*counter, zero_word.data(), zero_word.size());
    if (!status) {
        return fail(order_check, "clear counter buffer: " + status.to_string());
    }

    std::vector<uint64_t> batches;
    batches.reserve(kOrderedCommands);
    for (uint32_t index = 0; index < kOrderedCommands; ++index) {
        vkbridge::Command ordered_fill;
        ordered_fill.type = vkbridge::CommandType::BlitFill;
        ordered_fill.handle = *ordered;
        ordered_fill.offset = static_cast<uint64_t>(index) * 4;
        ordered_fill.size = 4;
        ordered_fill.value32 = 0xA0000000u + index;
        status = replay.apply(ordered_fill);
        if (!status) {
            return fail(order_check,
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
            return fail(order_check,
                        "record counter " + std::to_string(index) + ": " + status.to_string());
        }
        vkbridge::Result<uint64_t> sequence = replay.submit();
        if (!sequence) {
            return fail(order_check, "submit batch " + std::to_string(index) + ": " +
                                         sequence.status().to_string());
        }
        batches.push_back(*sequence);
    }
    if (replay.live_fences() != batches.size()) {
        return fail(order_check, "expected " + std::to_string(batches.size()) +
                                     " outstanding fences, found " +
                                     std::to_string(replay.live_fences()));
    }

    // "40 separate command buffers" is a real claim: every batch must still own a distinct
    // command buffer while all 40 submissions are outstanding.
    std::vector<VkCommandBuffer> command_buffers;
    command_buffers.reserve(batches.size());
    for (const uint64_t sequence : batches) {
        const vkbridge::Result<VkCommandBuffer> commands = replay.command_buffer_of(sequence);
        if (!commands) {
            return fail(order_check, "command buffer for sequence " + std::to_string(sequence) +
                                         ": " + commands.status().to_string());
        }
        command_buffers.push_back(*commands);
    }
    std::sort(command_buffers.begin(), command_buffers.end());
    if (std::adjacent_find(command_buffers.begin(), command_buffers.end()) !=
        command_buffers.end()) {
        return fail(order_check, "the 40 batches did not use 40 distinct command buffers");
    }

    // Poll the fences instead of waiting in submission order: the order the host observes
    // is the order the device actually signaled them.
    vkbridge::CompletionOrder completion;
    std::vector<bool> observed(batches.size(), false);
    std::vector<uint64_t> observed_order;
    observed_order.reserve(batches.size());
    size_t completed = 0;
    const std::chrono::steady_clock::time_point deadline =
        std::chrono::steady_clock::now() + kOrderingTimeout;
    while (completed < batches.size()) {
        bool progress = false;
        for (size_t index = 0; index < batches.size(); ++index) {
            if (observed[index]) {
                continue;
            }
            vkbridge::Result<bool> signaled = replay.fence_signaled(batches[index]);
            if (!signaled) {
                return fail(order_check, "poll batch " + std::to_string(index) + ": " +
                                             signaled.status().to_string());
            }
            if (!signaled.value()) {
                continue;
            }
            observed[index] = true;
            ++completed;
            observed_order.push_back(batches[index]);
            completion.observe(batches[index]);
            progress = true;
        }
        if (!progress) {
            if (std::chrono::steady_clock::now() >= deadline) {
                return fail(order_check, "timed out waiting for the batches to signal");
            }
            std::this_thread::sleep_for(std::chrono::microseconds(20));
        }
    }
    if (observed_order.size() != batches.size()) {
        return fail(order_check,
                    "the observed completion order is not a permutation of the batches");
    }
    std::vector<uint64_t> sorted_order = observed_order;
    std::sort(sorted_order.begin(), sorted_order.end());
    if (sorted_order != batches) {
        return fail(order_check,
                    "the observed completion order is not a permutation of the batches");
    }
    if (completion.inverted()) {
        return fail(order_check, "a completion was observed below an already completed batch");
    }
    for (size_t index = 1; index < observed_order.size(); ++index) {
        if (observed_order[index] <= observed_order[index - 1]) {
            return fail(order_check, "the device signaled batch " +
                                         std::to_string(observed_order[index]) + " before batch " +
                                         std::to_string(observed_order[index - 1]));
        }
    }

    for (const uint64_t sequence : batches) {
        status = replay.wait_fence(sequence, kFenceTimeoutNs);
        if (!status) {
            return fail(order_check, "wait for sequence " + std::to_string(sequence) + ": " +
                                         status.to_string());
        }
    }
    if (replay.live_fences() != 0) {
        return fail(order_check, "waited fences were not reaped: " +
                                     std::to_string(replay.live_fences()) + " still outstanding");
    }

    vkbridge::Result<std::vector<uint8_t>> ordered_data = replay.readback(*ordered, ordered_bytes);
    if (!ordered_data) {
        return fail(order_check, "readback ordered buffer: " + ordered_data.status().to_string());
    }
    vkbridge::Result<std::vector<uint8_t>> counter_data = replay.readback(*counter, 4);
    if (!counter_data) {
        return fail(order_check, "readback counter: " + counter_data.status().to_string());
    }
    for (uint32_t index = 0; index < kOrderedCommands; ++index) {
        const uint32_t expected = 0xA0000000u + index;
        if (vkbridge::read_le32(ordered_data->data() + index * 4) != expected) {
            return fail(order_check,
                        "word " + std::to_string(index) + " was not filled by its command buffer");
        }
    }
    if (vkbridge::read_le32(counter_data->data()) != kOrderedCommands) {
        return fail(order_check,
                    "counter ended at a value other than 40, so execution order was not preserved");
    }
    if (options.verbose) {
        std::cerr << "[info] submitted " << replay.submitted_batches()
                  << " batches; the polled completion order was ascending and all fences reaped\n";
    }
    std::cout << "[ok] 40 command buffers in order\n";

    // ------------------------------------------------------------------
    // fence signal/wait ordering
    // ------------------------------------------------------------------
    const std::string fence_check = "fence signal/wait ordering";
    vkbridge::Result<uint32_t> fence_buffer = replay.create_buffer(
        8, VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    if (!fence_buffer) {
        return fail(fence_check, "create fence buffer: " + fence_buffer.status().to_string());
    }
    status = replay.blit_fill(*fence_buffer, 0, 4, 0xB0000001u);
    if (!status) {
        return fail(fence_check, "record batch A: " + status.to_string());
    }
    vkbridge::Result<uint64_t> sequence_a = replay.submit();
    if (!sequence_a) {
        return fail(fence_check, "submit batch A: " + sequence_a.status().to_string());
    }
    status = replay.blit_fill(*fence_buffer, 4, 4, 0xB0000002u);
    if (!status) {
        return fail(fence_check, "record batch B: " + status.to_string());
    }
    vkbridge::Result<uint64_t> sequence_b = replay.submit();
    if (!sequence_b) {
        return fail(fence_check, "submit batch B: " + sequence_b.status().to_string());
    }
    if (*sequence_b != *sequence_a + 1) {
        return fail(fence_check, "submission sequence is not monotonic (" +
                                     std::to_string(*sequence_a) + " then " +
                                     std::to_string(*sequence_b) + ")");
    }
    status = replay.wait_fence(*sequence_b, kFenceTimeoutNs);
    if (!status) {
        return fail(fence_check, "later fence did not signal: " + status.to_string());
    }
    vkbridge::Result<bool> earlier_signaled = replay.fence_signaled(*sequence_a);
    if (!earlier_signaled || !earlier_signaled.value()) {
        return fail(fence_check, "earlier batch was still pending after the later one completed");
    }
    status = replay.wait_fence(*sequence_a, 0);
    if (!status) {
        return fail(fence_check,
                    "immediate wait on the earlier fence failed: " + status.to_string());
    }
    // Both submissions have been reaped now. Re-waiting a reaped sequence must still be a
    // success, and the reaped sequence must count as signaled.
    status = replay.wait_fence(*sequence_a, 0);
    if (!status) {
        return fail(fence_check,
                    "waiting again on a reaped sequence failed: " + status.to_string());
    }
    vkbridge::Result<bool> reaped_signaled = replay.fence_signaled(*sequence_b);
    if (!reaped_signaled || !reaped_signaled.value()) {
        return fail(fence_check, "a reaped sequence no longer counts as signaled");
    }

    vkbridge::Result<vkbridge::Fence> timeout_fence = vkbridge::Fence::create(device->handle(), 0);
    if (!timeout_fence) {
        return fail(fence_check, "create timeout fence: " + timeout_fence.status().to_string());
    }
    const vkbridge::Status timeout_status = timeout_fence->wait(1'000'000);
    if (timeout_status.code() != vkbridge::Error::Timeout) {
        return fail(fence_check, "waiting for an unsignaled fence did not report a timeout");
    }

    vkbridge::Result<std::vector<uint8_t>> fence_data = replay.readback(*fence_buffer, 8);
    if (!fence_data) {
        return fail(fence_check, "readback: " + fence_data.status().to_string());
    }
    if (vkbridge::read_le32(fence_data->data()) != 0xB0000001u ||
        vkbridge::read_le32(fence_data->data() + 4) != 0xB0000002u) {
        return fail(fence_check, "fence batches did not reach the buffer");
    }
    if (replay.live_fences() != 0) {
        return fail(fence_check, "waited fences were not reaped: " +
                                     std::to_string(replay.live_fences()) + " still outstanding");
    }
    std::cout << "[ok] fence signal/wait ordering\n";

    // ------------------------------------------------------------------
    // screenshot: render, device-side attestation, PNG decode and comparison
    // ------------------------------------------------------------------
    const std::string screenshot_check = "screenshot " + options.screenshot_path;
    vkbridge::ScreenshotOptions screenshot_options;
    screenshot_options.attestation_nonce = kScreenshotAttestationNonce;
    vkbridge::Result<vkbridge::ScreenshotInfo> screenshot =
        replay.screenshot(options.screenshot_path, screenshot_options);
    if (!screenshot) {
        return fail(screenshot_check, screenshot.status().to_string());
    }
    memory_problem =
        verify_attestation(screenshot->attestation, screenshot->rgba.data(),
                           screenshot->rgba.size(), screenshot->attestation_nonce, "screenshot");
    if (!memory_problem.empty()) {
        return fail(screenshot_check, memory_problem);
    }
    if (!screenshot->render_target_device_local) {
        return fail(screenshot_check,
                    "the screenshot path reported no device-local render target, so the host "
                    "could have produced the frame without the device");
    }
    if (segregated_device_local && screenshot->render_target_host_visible) {
        return fail(screenshot_check,
                    "the render target is host-visible even though this device exposes a "
                    "non-mappable device-local memory type");
    }
    if (options.verbose) {
        std::cerr << "[info] screenshot attestation: checksum "
                  << format_hex32(screenshot->attestation.checksum) << ", fingerprint "
                  << format_hex32(screenshot->attestation.fingerprint) << "\n";
    }

    const vkbridge::ScreenshotLayout layout =
        vkbridge::make_screenshot_layout(screenshot->width, screenshot->height);
    const std::vector<uint8_t> reference = vkbridge::render_layout(layout);
    if (reference.size() != screenshot->rgba.size()) {
        return fail(screenshot_check, "GPU frame has an unexpected size");
    }
    if (reference != screenshot->rgba) {
        return fail(screenshot_check, "GPU frame differs from the CPU layout model");
    }
    const size_t reference_unique =
        vkbridge::count_unique_colours(reference.data(), reference.size() / 4);
    if (screenshot->unique_colours != reference_unique) {
        return fail(screenshot_check, "unique colour count does not match the CPU layout model");
    }

    // Decode the file that was actually written: signature, chunk CRCs, stored-block
    // framing and Adler-32, then compare its pixels with the CPU model.
    vkbridge::PngPixels decoded;
    const vkbridge::Status png_status = vkbridge::read_png(options.screenshot_path, decoded);
    if (!png_status) {
        return fail(screenshot_check, "decode the written PNG: " + png_status.to_string());
    }
    if (decoded.width != screenshot->width || decoded.height != screenshot->height) {
        return fail(screenshot_check, "the written PNG has different dimensions");
    }
    if (decoded.rgba != reference) {
        return fail(screenshot_check, "the PNG pixels differ from the CPU layout model");
    }

    std::cout << "[ok] screenshot " << options.screenshot_path << " (" << screenshot->width << "x"
              << screenshot->height << ", " << screenshot->unique_colours
              << " unique colours, PNG verified, device checksum ok)\n";
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
        return kExitUsage;
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
