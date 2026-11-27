#pragma once

// A named shared-memory segment: shm_open + ftruncate + mmap, RAII.
//
// Two backings:
//   * /dev/shm (tmpfs) via shm_open           -- always available
//   * /dev/hugepages (hugetlbfs, 2 MiB pages) -- only if the admin reserved
//     pages (vm.nr_hugepages > 0); MAP_HUGETLB cannot be used on a tmpfs fd,
//     so huge pages for *shared* memory mean a hugetlbfs file.
//
// HugePages::Try falls back to tmpfs and says so via huge(); the caller can
// prove what it actually got with page_size_kb(), which reads /proc/self/smaps
// rather than trusting the flag.
//
// The creator owns the name and unlinks it on destruction; attachers only
// unmap. Nothing inside the segment may be a pointer: every process maps it
// at a different address, so the data structures use indices only.

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <utility>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace shmring {

enum class HugePages { No, Try, Require };

class ShmSegment {
public:
    static constexpr std::size_t kHugePage = 2u << 20;

    ShmSegment() = default;
    ShmSegment(const ShmSegment&) = delete;
    ShmSegment& operator=(const ShmSegment&) = delete;
    ShmSegment(ShmSegment&& o) noexcept { *this = std::move(o); }
    ShmSegment& operator=(ShmSegment&& o) noexcept {
        if (this != &o) {
            reset();
            name_ = std::move(o.name_);
            addr_ = std::exchange(o.addr_, nullptr);
            size_ = std::exchange(o.size_, 0);
            owner_ = std::exchange(o.owner_, false);
            huge_ = std::exchange(o.huge_, false);
        }
        return *this;
    }
    ~ShmSegment() { reset(); }

    // Create (replacing any stale segment of the same name left by a crash).
    static ShmSegment create(const std::string& name, std::size_t size, HugePages hp = HugePages::No) {
        ShmSegment s;
        s.name_ = name;
        s.owner_ = true;
        if (hp != HugePages::No) {
            const std::size_t hsize = (size + kHugePage - 1) & ~(kHugePage - 1);
            const std::string path = huge_path(name);
            ::unlink(path.c_str());
            const int fd = ::open(path.c_str(), O_CREAT | O_EXCL | O_RDWR, 0600);
            if (fd >= 0) {
                void* p = MAP_FAILED;
                if (::ftruncate(fd, off_t(hsize)) == 0)
                    p = ::mmap(nullptr, hsize, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, fd, 0);
                ::close(fd);
                if (p != MAP_FAILED) {
                    s.addr_ = p;
                    s.size_ = hsize;
                    s.huge_ = true;
                    return s;
                }
                ::unlink(path.c_str());  // usually ENOMEM: no pages reserved
            }
            if (hp == HugePages::Require)
                throw std::runtime_error("shm-ring: huge pages required but unavailable (" + path +
                                         "): is vm.nr_hugepages > 0?");
        }
        const std::string sname = "/" + name;
        ::shm_unlink(sname.c_str());
        const int fd = ::shm_open(sname.c_str(), O_CREAT | O_EXCL | O_RDWR, 0600);
        if (fd < 0) fail("shm_open(create)", sname);
        if (::ftruncate(fd, off_t(size)) != 0) {
            ::close(fd);
            fail("ftruncate", sname);
        }
        // MAP_POPULATE pre-faults every page now, so the first message through
        // the ring does not pay a page fault on the hot path.
        void* p = ::mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, fd, 0);
        ::close(fd);
        if (p == MAP_FAILED) fail("mmap", sname);
        s.addr_ = p;
        s.size_ = size;
        return s;
    }

    // Attach to an existing segment (tmpfs first, then hugetlbfs). Returns an
    // empty segment (!valid()) if neither exists yet -- callers poll.
    static ShmSegment open(const std::string& name) {
        ShmSegment s;
        s.name_ = name;
        const std::string sname = "/" + name;
        int fd = ::shm_open(sname.c_str(), O_RDWR, 0600);
        if (fd < 0) {
            fd = ::open(huge_path(name).c_str(), O_RDWR);
            if (fd < 0) return s;
            s.huge_ = true;
        }
        struct stat st {};
        if (::fstat(fd, &st) != 0 || st.st_size == 0) {  // creator has not sized it yet
            ::close(fd);
            return ShmSegment{};
        }
        void* p = ::mmap(nullptr, std::size_t(st.st_size), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE,
                         fd, 0);
        ::close(fd);
        if (p == MAP_FAILED) fail("mmap(attach)", sname);
        s.addr_ = p;
        s.size_ = std::size_t(st.st_size);
        return s;
    }

    bool valid() const noexcept { return addr_ != nullptr; }
    void* data() const noexcept { return addr_; }
    std::size_t size() const noexcept { return size_; }
    bool huge() const noexcept { return huge_; }
    bool owner() const noexcept { return owner_; }
    const std::string& name() const noexcept { return name_; }

    // Kernel page size backing this mapping, from /proc/self/smaps. 4 = normal,
    // 2048 = huge. Returns 0 if not found.
    std::size_t page_size_kb() const {
        FILE* f = std::fopen("/proc/self/smaps", "r");
        if (!f) return 0;
        char line[512];
        bool in = false;
        std::size_t kb = 0;
        const auto base = reinterpret_cast<unsigned long>(addr_);
        while (std::fgets(line, sizeof line, f)) {
            unsigned long lo, hi;
            if (std::sscanf(line, "%lx-%lx ", &lo, &hi) == 2 && std::strchr(line, '-') < std::strchr(line, ' ')) {
                in = (lo == base);
                continue;
            }
            if (in && std::sscanf(line, "KernelPageSize: %zu kB", &kb) == 1) break;
        }
        std::fclose(f);
        return kb;
    }

private:
    static std::string huge_path(const std::string& name) { return "/dev/hugepages/" + name; }

    [[noreturn]] static void fail(const char* what, const std::string& name) {
        throw std::runtime_error(std::string("shm-ring: ") + what + " " + name + ": " + std::strerror(errno));
    }

    void reset() noexcept {
        if (addr_) ::munmap(addr_, size_);
        if (owner_ && !name_.empty()) {
            if (huge_) ::unlink(huge_path(name_).c_str());
            else ::shm_unlink(("/" + name_).c_str());
        }
        addr_ = nullptr;
        size_ = 0;
        owner_ = false;
    }

    std::string name_;
    void* addr_ = nullptr;
    std::size_t size_ = 0;
    bool owner_ = false;
    bool huge_ = false;
};

}  // namespace shmring
