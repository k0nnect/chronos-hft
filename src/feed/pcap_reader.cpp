// PcapReader: the cold mmap setup/teardown path kept out of the headers so the
// posix mapping syscalls do not leak into every translation unit that wants to
// iterate packets. all of the hot-path work lives inline in pcap_reader.hpp.
#include "hft/feed/pcap_reader.hpp"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstddef>
#include <cstdint>

#include "hft/feed/pcap_structures.hpp"

namespace hft {

bool PcapReader::open(const char* path) noexcept {
    close();  // a re-open never leaks the previous mapping
    if (path == nullptr) {
        return false;
    }

    const int fd = ::open(path, O_RDONLY);
    if (fd < 0) {
        return false;
    }

    struct stat st {};
    if (::fstat(fd, &st) != 0 || st.st_size < 0) {
        ::close(fd);
        return false;
    }
    const std::size_t file_size = static_cast<std::size_t>(st.st_size);
    if (file_size < pcap::global_header_size) {
        ::close(fd);  // too small to even hold a global header
        return false;
    }

    void* mapping = ::mmap(nullptr, file_size, PROT_READ, MAP_PRIVATE, fd, 0);
    // the descriptor is not needed once the mapping exists; the pages stay valid.
    ::close(fd);
    if (mapping == MAP_FAILED) {
        return false;
    }

    auto* const base = static_cast<std::uint8_t*>(mapping);

    // validate the global header before exposing anything. an unknown magic or a
    // non-ethernet link type means we cannot peel frames -- unmap & report failure.
    bool          swapped = false;
    bool          nanos   = false;
    const std::uint32_t magic = pcap::read_meta_u32(base + pcap::gh_off_magic, /*swapped=*/false);
    if (!pcap::classify_magic(magic, swapped, nanos)) {
        ::munmap(mapping, file_size);
        return false;
    }
    const std::uint32_t network = pcap::read_meta_u32(base + pcap::gh_off_network, swapped);
    if (network != pcap::linktype_ethernet) {
        ::munmap(mapping, file_size);
        return false;
    }

    // the trace is read strictly front-to-back; tell the kernel so it can
    // read-ahead aggressively. advisory only -- ignore platforms without it.
#if defined(MADV_SEQUENTIAL)
    (void)::madvise(mapping, file_size, MADV_SEQUENTIAL);
#endif

    base_      = base;
    size_      = file_size;
    swapped_   = swapped;
    nanos_     = nanos;
    link_type_ = network;
    return true;
}

void PcapReader::close() noexcept {
    if (base_ != nullptr) {
        ::munmap(base_, size_);
    }
    base_      = nullptr;
    size_      = 0;
    swapped_   = false;
    nanos_     = false;
    link_type_ = 0;
}

}  // namespace hft
