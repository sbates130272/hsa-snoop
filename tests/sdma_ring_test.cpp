#include "parser.h"

#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <thread>
#include <vector>

namespace {

constexpr uint64_t kSrcAddress = 0x100000000ULL;
constexpr uint64_t kDstAddress = 0x200000000ULL;

void WriteSequence(uint32_t* dst, uint32_t copy_header, uint32_t count,
                   uint32_t copy_dwords) {
    // COPY_LINEAR and COPY_LINEAR_BC share the first seven dwords. A broadcast
    // COPY_LINEAR appends a second destination address in dwords 7-8.
    dst[0] = copy_header;
    dst[1] = count;
    dst[2] = 0;
    dst[3] = static_cast<uint32_t>(kSrcAddress);
    dst[4] = static_cast<uint32_t>(kSrcAddress >> 32);
    dst[5] = static_cast<uint32_t>(kDstAddress);
    dst[6] = static_cast<uint32_t>(kDstAddress >> 32);
    for (uint32_t i = 7; i < copy_dwords; ++i)
        dst[i] = 0;

    // FENCE: 4 dwords.
    dst[copy_dwords] = sdma::OP_FENCE;
    dst[copy_dwords + 1] = 0;
    dst[copy_dwords + 2] = 0;
    dst[copy_dwords + 3] = 0;

    // TRAP: 2 dwords.
    dst[copy_dwords + 4] = sdma::OP_TRAP;
    dst[copy_dwords + 5] = 0;
}

bool RunParserTest(sdma::Version version, uint8_t copy_sub_op, bool broadcast,
                   uint32_t count, uint64_t expected_bytes,
                   const char* expected_name, bool expect_records) {
    // This is an in-process stand-in for a KFD SDMA queue. The unused ring is
    // zero-filled, exactly like a newly allocated ROCR ring; reading past the
    // byte-valued wptr would therefore produce a stream of false NOP packets.
    std::vector<uint32_t> ring(1024, 0);
    alignas(uint64_t) std::atomic<uint64_t> wptr_bytes{0};
    alignas(uint64_t) std::atomic<uint64_t> rptr_bytes{0};

    std::mutex records_mu;
    std::vector<hsasnoop::SdmaRecord> records;
    hsasnoop::RingParser parser(
        {},
        [&](const hsasnoop::SdmaRecord& record) {
            std::lock_guard<std::mutex> lock(records_mu);
            records.push_back(record);
        },
        1000);

    hsasnoop::QueueInfo queue;
    queue.pid = getpid();
    queue.ring_base = reinterpret_cast<uint64_t>(ring.data());
    queue.wptr_addr = reinterpret_cast<uint64_t>(&wptr_bytes);
    queue.rptr_addr = reinterpret_cast<uint64_t>(&rptr_bytes);
    queue.ring_size = static_cast<uint32_t>(ring.size() * sizeof(uint32_t));
    queue.qtype = 1; // KFD_IOC_QUEUE_TYPE_SDMA
    queue.uid = static_cast<uint64_t>(version) + 1;
    queue.sdma_version = version;
    parser.AddQueue(queue);

    const uint32_t copy_header = sdma::OP_COPY |
                                 (static_cast<uint32_t>(copy_sub_op) << 8) |
                                 (broadcast ? (1u << 27) : 0);
    const uint32_t copy_dwords = broadcast ? 9 : 7;
    const uint32_t sequence_dwords = copy_dwords + 6;
    const uint64_t sequence_bytes = sequence_dwords * sizeof(uint32_t);

    // Publish several sequences so the test does not depend on whether the
    // parser thread primes immediately before or during the first iteration.
    // Only the newly committed sequence is populated; an implementation that
    // mistakes byte pointers for dword pointers will walk into the zero-filled
    // uncommitted area and emit NOPs.
    for (uint64_t i = 0; i < 16; ++i) {
        WriteSequence(ring.data() + i * sequence_dwords, copy_header, count,
                      copy_dwords);
        const uint64_t committed_bytes = (i + 1) * sequence_bytes;
        wptr_bytes.store(committed_bytes, std::memory_order_release);
        rptr_bytes.store(committed_bytes, std::memory_order_release);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    parser.Stop();

    size_t copies = 0;
    size_t fences = 0;
    size_t traps = 0;
    size_t nops = 0;
    size_t unexpected = 0;
    bool bad_copy = false;
    {
        std::lock_guard<std::mutex> lock(records_mu);
        for (const auto& record : records) {
            switch (record.opcode) {
            case sdma::OP_COPY:
                ++copies;
                if (record.sdma_version != version ||
                    record.bytes != expected_bytes ||
                    record.src_addr != kSrcAddress ||
                    record.dst_addr != kDstAddress ||
                    record.op_name != expected_name)
                    bad_copy = true;
                break;
            case sdma::OP_FENCE:
                ++fences;
                break;
            case sdma::OP_TRAP:
                ++traps;
                break;
            case sdma::OP_NOP:
                ++nops;
                break;
            default:
                ++unexpected;
                break;
            }
        }
    }

    if (!expect_records)
        return records.empty();

    if (copies == 0 || copies != fences || copies != traps || nops != 0 ||
        unexpected != 0 || bad_copy) {
        std::fprintf(stderr,
                     "unexpected SDMA %s decode: copy=%zu fence=%zu trap=%zu "
                     "nop=%zu other=%zu bad_copy=%d\n",
                     sdma::VersionName(version), copies, fences, traps, nops,
                     unexpected, bad_copy);
        return false;
    }
    return true;
}

} // namespace

int main() {
    constexpr uint32_t kCountWithV6HighBit = 0x04000003;
    if (!RunParserTest(sdma::Version::V4, sdma::SUBOP_COPY_LINEAR, false,
                       kCountWithV6HighBit, 4, "copy_linear", true))
        return 1;
    if (!RunParserTest(sdma::Version::V6, sdma::SUBOP_COPY_LINEAR, false,
                       kCountWithV6HighBit, 0x04000004ULL, "copy_linear", true))
        return 1;
    if (!RunParserTest(sdma::Version::V6, sdma::SUBOP_COPY_LINEAR_BC, false,
                       kCountWithV6HighBit, 4, "copy_linear_bc", true))
        return 1;
    if (!RunParserTest(sdma::Version::V6, sdma::SUBOP_COPY_LINEAR, true,
                       kCountWithV6HighBit, 0x04000004ULL,
                       "copy_linear_broadcast", true))
        return 1;
    if (!RunParserTest(sdma::Version::Unknown, sdma::SUBOP_COPY_LINEAR, false,
                       3, 0, "copy_linear", false)) {
        std::fprintf(stderr, "unsupported SDMA version produced records\n");
        return 1;
    }
    return 0;
}
