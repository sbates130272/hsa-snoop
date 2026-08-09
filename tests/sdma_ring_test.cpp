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

constexpr uint32_t kSequenceDwords = 13;
constexpr uint64_t kSequenceBytes = kSequenceDwords * sizeof(uint32_t);

void WriteSequence(uint32_t* dst) {
    // COPY_LINEAR: 7 dwords.
    dst[0] = sdma::OP_COPY;
    dst[1] = 3; // four bytes
    dst[2] = 0;
    dst[3] = 0;
    dst[4] = 0;
    dst[5] = 0;
    dst[6] = 0;

    // FENCE: 4 dwords.
    dst[7] = sdma::OP_FENCE;
    dst[8] = 0;
    dst[9] = 0;
    dst[10] = 0;

    // TRAP: 2 dwords.
    dst[11] = sdma::OP_TRAP;
    dst[12] = 0;
}

} // namespace

int main() {
    // This is an in-process stand-in for a KFD SDMA queue. The unused ring is
    // zero-filled, exactly like a newly allocated ROCR ring; reading past the
    // byte-valued wptr would therefore produce a stream of false NOP packets.
    std::vector<uint32_t> ring(1024, 0);
    alignas(uint64_t) std::atomic<uint64_t> wptr_bytes{0};
    alignas(uint64_t) std::atomic<uint64_t> rptr_bytes{0};

    std::mutex records_mu;
    std::vector<uint8_t> opcodes;
    hsasnoop::RingParser parser(
        {},
        [&](const hsasnoop::SdmaRecord& record) {
            std::lock_guard<std::mutex> lock(records_mu);
            opcodes.push_back(record.opcode);
        },
        1000);

    hsasnoop::QueueInfo queue;
    queue.pid = getpid();
    queue.ring_base = reinterpret_cast<uint64_t>(ring.data());
    queue.wptr_addr = reinterpret_cast<uint64_t>(&wptr_bytes);
    queue.rptr_addr = reinterpret_cast<uint64_t>(&rptr_bytes);
    queue.ring_size = static_cast<uint32_t>(ring.size() * sizeof(uint32_t));
    queue.qtype = 1; // KFD_IOC_QUEUE_TYPE_SDMA
    queue.uid = 1;
    parser.AddQueue(queue);

    // Publish several sequences so the test does not depend on whether the
    // parser thread primes immediately before or during the first iteration.
    // Only the newly committed sequence is populated; an implementation that
    // mistakes byte pointers for dword pointers will walk into the zero-filled
    // uncommitted area and emit NOPs.
    for (uint64_t i = 0; i < 16; ++i) {
        WriteSequence(ring.data() + i * kSequenceDwords);
        const uint64_t committed_bytes = (i + 1) * kSequenceBytes;
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
    {
        std::lock_guard<std::mutex> lock(records_mu);
        for (uint8_t opcode : opcodes) {
            switch (opcode) {
            case sdma::OP_COPY:
                ++copies;
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

    if (copies == 0 || copies != fences || copies != traps || nops != 0 ||
        unexpected != 0) {
        std::fprintf(stderr,
                     "unexpected SDMA decode: copy=%zu fence=%zu trap=%zu "
                     "nop=%zu other=%zu\n",
                     copies, fences, traps, nops, unexpected);
        return 1;
    }
    return 0;
}
