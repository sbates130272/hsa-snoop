#include "gpu_clock.h"

#include <fcntl.h>
#include <linux/kfd_ioctl.h>
#include <sys/ioctl.h>
#include <unistd.h>

namespace hsasnoop {

bool MeasureGpuClockHz(uint32_t gpu_id, double* hz) {
    // Opening /dev/kfd registers this process with the driver for as long as
    // the descriptor is open, which is why it is held only for the measurement.
    int fd = open("/dev/kfd", O_RDWR | O_CLOEXEC);
    if (fd < 0)
        return false;

    struct kfd_ioctl_get_clock_counters_args a {
    }, b{};
    a.gpu_id = b.gpu_id = gpu_id;
    bool ok = ioctl(fd, AMDKFD_IOC_GET_CLOCK_COUNTERS, &a) == 0;
    if (ok) {
        usleep(200 * 1000);
        ok = ioctl(fd, AMDKFD_IOC_GET_CLOCK_COUNTERS, &b) == 0;
    }
    close(fd);
    if (!ok)
        return false;

    // cpu_clock_counter is the raw monotonic clock in nanoseconds.
    if (b.gpu_clock_counter <= a.gpu_clock_counter ||
        b.cpu_clock_counter <= a.cpu_clock_counter)
        return false;
    *hz = static_cast<double>(b.gpu_clock_counter - a.gpu_clock_counter) * 1e9 /
          static_cast<double>(b.cpu_clock_counter - a.cpu_clock_counter);
    return *hz > 0;
}

} // namespace hsasnoop
