#include "host_storage.h"

#include <dirent.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <vector>

namespace sim {

namespace {
bool isDir(const std::string& p) {
    struct stat st;
    return ::stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}
bool pathExists(const std::string& p) {
    struct stat st;
    return ::stat(p.c_str(), &st) == 0;
}
bool mkdirs(const std::string& p) {
    if (p.empty() || isDir(p)) return true;
    const size_t slash = p.find_last_of('/');
    if (slash != std::string::npos && slash > 0 && !mkdirs(p.substr(0, slash))) return false;
    return ::mkdir(p.c_str(), 0755) == 0 || isDir(p);
}
}  // namespace

bool HostStorage::init(const std::string& root) {
    root_ = root;
    while (root_.size() > 1 && root_.back() == '/') root_.pop_back();
    return mkdirs(root_) && isDir(root_);
}

bool HostStorage::resolve(const char* path, std::string& out, bool* existsOut) const {
    out = root_;
    bool exists = true;
    std::string p(path ? path : "");
    size_t i = 0;
    while (i < p.size()) {
        while (i < p.size() && p[i] == '/') ++i;
        if (i >= p.size()) break;
        size_t j = p.find('/', i);
        if (j == std::string::npos) j = p.size();
        const std::string comp = p.substr(i, j - i);
        i = j;
        if (comp == "." ) continue;
        if (comp == "..") return false;   // not on a FAT card root
        if (!exists) return false;        // missing intermediate directory
        std::string cand = out + "/" + comp;
        if (pathExists(cand)) {
            out = cand;
            continue;
        }
        // FAT semantics: case-insensitive match.
        bool found = false;
        if (DIR* d = ::opendir(out.c_str())) {
            while (dirent* e = ::readdir(d)) {
                if (::strcasecmp(e->d_name, comp.c_str()) == 0) {
                    out += "/";
                    out += e->d_name;
                    found = true;
                    break;
                }
            }
            ::closedir(d);
        }
        if (!found) {
            out = cand;
            exists = false;
        }
    }
    if (existsOut) *existsOut = exists;
    return true;
}

ot::IStorage::Handle HostStorage::open(const char* path, OpenMode mode) {
    std::string hp;
    bool ex = false;
    if (!resolve(path, hp, &ex)) return -1;
    if (ex && isDir(hp)) return -1;
    int slot = -1;
    for (int i = 0; i < kMaxOpen; ++i)
        if (!files_[i]) { slot = i; break; }
    if (slot < 0) return -1;
    std::FILE* f = nullptr;
    switch (mode) {
        case kRead: f = ex ? std::fopen(hp.c_str(), "rb") : nullptr; break;
        case kWrite: f = std::fopen(hp.c_str(), "wb"); break;
        case kReadWrite: f = std::fopen(hp.c_str(), ex ? "r+b" : "w+b"); break;
    }
    if (!f) return -1;
    files_[slot] = f;
    return slot;
}

int32_t HostStorage::read(Handle h, void* dst, uint32_t len) {
    std::FILE* f = get(h);
    if (!f) return -1;
    std::fflush(f);
    const size_t n = std::fread(dst, 1, len, f);
    if (n < len && std::ferror(f)) return -1;
    return int32_t(n);
}

int32_t HostStorage::write(Handle h, const void* src, uint32_t len) {
    std::FILE* f = get(h);
    if (!f) return -1;
    // Switching from reading to writing on a "r+" stream needs a positioning call.
    std::fseek(f, 0, SEEK_CUR);
    const size_t n = std::fwrite(src, 1, len, f);
    return n == len || n > 0 ? int32_t(n) : -1;
}

bool HostStorage::seek(Handle h, uint32_t pos) {
    std::FILE* f = get(h);
    return f && std::fseek(f, long(pos), SEEK_SET) == 0;
}

int32_t HostStorage::size(Handle h) {
    std::FILE* f = get(h);
    if (!f) return -1;
    std::fflush(f);
    struct stat st;
    if (::fstat(::fileno(f), &st) != 0) return -1;
    return int32_t(st.st_size);
}

void HostStorage::close(Handle h) {
    if (std::FILE* f = get(h)) {
        std::fclose(f);
        files_[h] = nullptr;
    }
}

bool HostStorage::exists(const char* path) {
    std::string hp;
    bool ex = false;
    return resolve(path, hp, &ex) && ex;
}

bool HostStorage::mkdir(const char* path) {
    std::string hp;
    bool ex = false;
    if (!resolve(path, hp, &ex)) return false;
    if (ex) return isDir(hp);
    return ::mkdir(hp.c_str(), 0755) == 0 || isDir(hp);
}

bool HostStorage::remove(const char* path) {
    std::string hp;
    bool ex = false;
    if (!resolve(path, hp, &ex) || !ex || hp == root_) return false;
    return isDir(hp) ? ::rmdir(hp.c_str()) == 0 : ::unlink(hp.c_str()) == 0;
}

bool HostStorage::rename(const char* from, const char* to) {
    std::string a, b;
    bool exA = false, exB = false;
    if (!resolve(from, a, &exA) || !exA || !resolve(to, b, &exB)) return false;
    if (exB && a != b) return false;   // FatFs f_rename fails if the target exists
    return std::rename(a.c_str(), b.c_str()) == 0;
}

int32_t HostStorage::listDir(const char* path, DirFn fn, void* ctx) {
    std::string hp;
    bool ex = false;
    if (!resolve(path, hp, &ex) || !ex || !isDir(hp)) return -1;
    struct Ent {
        std::string name;
        bool dir;
        uint32_t size;
    };
    std::vector<Ent> ents;
    if (DIR* d = ::opendir(hp.c_str())) {
        while (dirent* e = ::readdir(d)) {
            const std::string n = e->d_name;
            if (n == "." || n == "..") continue;
            struct stat st;
            if (::stat((hp + "/" + n).c_str(), &st) != 0) continue;
            ents.push_back({n, S_ISDIR(st.st_mode), S_ISDIR(st.st_mode) ? 0u : uint32_t(st.st_size)});
        }
        ::closedir(d);
    } else {
        return -1;
    }
    std::sort(ents.begin(), ents.end(), [](const Ent& x, const Ent& y) { return x.name < y.name; });
    int32_t n = 0;
    for (const Ent& e : ents) {
        DirEntry de{e.name.c_str(), e.dir, e.size};
        ++n;
        if (fn && !fn(de, ctx)) break;
    }
    return n;
}

}  // namespace sim
