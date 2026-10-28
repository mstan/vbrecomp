#ifndef VB_SNAPSHOT_PAGES_H
#define VB_SNAPSHOT_PAGES_H
/* Private guest-thread-owned immutable history pages. Public state files keep
 * their contiguous checked format. Sample fingerprints only select a bounded
 * candidate set: equality always requires all bytes to compare equal.
 */
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <unordered_map>
#include <vector>

namespace vb_history {
using std::size_t;
constexpr size_t page_size=4096;
struct PageCosts {
    uint64_t live_pages=0,peak_pages=0,allocated_pages=0,copied_bytes=0,hits=0,comparison_span_bytes=0;
};
struct Page {
    std::shared_ptr<PageCosts> costs;
    size_t size;
    std::array<uint8_t,page_size> bytes;
    Page(std::shared_ptr<PageCosts> c,const uint8_t* data,size_t n):costs(std::move(c)),size(n) {
        std::memcpy(bytes.data(),data,n);
        ++costs->live_pages;++costs->allocated_pages;costs->copied_bytes+=n;
        costs->peak_pages=std::max(costs->peak_pages,costs->live_pages);
    }
    ~Page(){--costs->live_pages;}
};
using PageRef=std::shared_ptr<const Page>;
class PagePool {
    struct Candidate {std::weak_ptr<const Page> page;unsigned hits=0;};
    using Bucket=std::vector<Candidate>;
    std::unordered_map<uint64_t,Bucket> index;
    std::shared_ptr<PageCosts> counters=std::make_shared<PageCosts>();
    size_t bucket_limit;
    static uint64_t word(const uint8_t* p){uint64_t v;std::memcpy(&v,p,8);return v;}
public:
    explicit PagePool(size_t limit=65536):bucket_limit(std::max(size_t(1),limit)){}
    std::shared_ptr<const PageCosts> costs() const{return counters;}
    size_t indexed_buckets() const{return index.size();}
    static uint64_t fingerprint(const uint8_t* p,size_t n) {
        uint64_t h=uint64_t(n)*0x9e3779b97f4a7c15ull;
        if(n>=8) {
            for(size_t offset:{size_t(0),(n-8)/3,(n-8)*2/3,n-8}) {
                h^=word(p+offset)+0x9e3779b97f4a7c15ull+(h<<6)+(h>>2);
            }
        } else for(size_t i=0;i<n;++i)h=(h^p[i])*0x100000001b3ull;
        return h;
    }
    PageRef intern(const uint8_t* data,size_t n) {
        if(!n || n>page_size)throw std::invalid_argument("Invalid history page size");
        const uint64_t key=fingerprint(data,n);
        auto found=index.find(key);
        if(found!=index.end()) {
            auto& bucket=found->second;
            for(auto it=bucket.begin();it!=bucket.end();) {
                auto page=it->page.lock();
                if(!page){it=bucket.erase(it);continue;}
                counters->comparison_span_bytes+=n;
                if(page->size==n && !std::memcmp(page->bytes.data(),data,n)) {
                    it->hits=std::min(255u,it->hits+1);++counters->hits;return page;
                }
                ++it;
            }
        } else {
            // The lookup table owns no pages. Evicting a live weak entry only
            // loses a possible cache hit, never snapshot bytes or ownership.
            if(index.size()>=bucket_limit)index.erase(index.begin());
            found=index.emplace(key,Bucket{}).first;
        }
        // Separate Page/control allocations: make_shared would retain the
        // full 4KiB allocation until every cached weak reference expires.
        PageRef page(new Page(counters,data,n));
        auto& bucket=found->second;
        if(bucket.size()>=4) {
            auto victim=std::min_element(bucket.begin(),bucket.end(),
                [](const Candidate& a,const Candidate& b){return a.hits<b.hits;});
            *victim={page,0};
        } else bucket.push_back({page,0});
        return page;
    }
};
struct Snapshot {
    struct Part {size_t offset;PageRef page;};
    std::vector<Part> parts;
    size_t size=0;
    bool read(size_t offset,void* destination,size_t count) const {
        if(offset>size || count>size-offset || (!destination && count))return false;
        if(!count)return true;
        auto it=std::upper_bound(parts.begin(),parts.end(),offset,
            [](size_t at,const Part& p){return at<p.offset;});
        if(it==parts.begin())return false;
        --it;auto* out=static_cast<uint8_t*>(destination);
        while(count) {
            if(it==parts.end() || offset<it->offset || offset-it->offset>=it->page->size)return false;
            const size_t local=offset-it->offset;
            const size_t n=std::min(count,it->page->size-local);
            std::memcpy(out,it->page->bytes.data()+local,n);
            out+=n;offset+=n;count-=n;++it;
        }
        return true;
    }
};
class Builder {
    PagePool& pool;
    Snapshot snapshot;
    std::array<uint8_t,page_size> pending;
    size_t used=0;
    void flush() {
        if(!used)return;
        snapshot.parts.push_back({snapshot.size,pool.intern(pending.data(),used)});
        snapshot.size+=used;used=0;
    }
public:
    explicit Builder(PagePool& p):pool(p){}
    void append(const void* bytes,size_t n) {
        if(n && !bytes)throw std::invalid_argument("Null history input");
        const auto* in=static_cast<const uint8_t*>(bytes);
        // Large visited regions get natural page boundaries even when a
        // preceding scalar field or prior region has a short tail.
        if(n>=page_size)flush();
        while(n) {
            if(!used && n>=page_size) {
                snapshot.parts.push_back({snapshot.size,pool.intern(in,page_size)});
                snapshot.size+=page_size;in+=page_size;n-=page_size;continue;
            }
            const size_t take=std::min(n,page_size-used);
            std::memcpy(pending.data()+used,in,take);used+=take;in+=take;n-=take;
            if(used==page_size)flush();
        }
    }
    Snapshot finish(){flush();return std::move(snapshot);}
};
class Ring {
    struct Slot {uint32_t tick=0;std::unique_ptr<Snapshot> snapshot;};
    std::vector<Slot> slots;
    unsigned head=0,occupied=0;
    static unsigned checked_depth(unsigned n) {
        if(!n || n>512)throw std::invalid_argument("Invalid history depth");
        return n;
    }
public:
    PagePool pool;
    explicit Ring(unsigned depth):slots(checked_depth(depth)){}
    const Snapshot* peek(uint32_t tick) const {
        for(const auto& s:slots)if(s.snapshot && s.tick==tick)return s.snapshot.get();
        return nullptr;
    }
    unsigned count() const {return occupied;}
    void clear(){for(auto& s:slots)s.snapshot.reset();head=occupied=0;}
    void store(uint32_t tick,Snapshot snapshot) {
        if(!snapshot.size)throw std::invalid_argument("Empty history snapshot");
        // Allocate the replacement before evicting anything. Allocation
        // failure leaves retained history intact.
        auto owner=std::make_unique<Snapshot>(std::move(snapshot));
        unsigned target=unsigned(slots.size());
        for(unsigned i=0;i<slots.size();++i)
            if(slots[i].snapshot && slots[i].tick==tick){target=i;break;}
        if(target==slots.size())for(unsigned i=0;i<slots.size();++i) {
            const unsigned at=(head+i)%unsigned(slots.size());
            if(!slots[at].snapshot){target=at;break;}
        }
        if(target==slots.size()) {
            uint32_t oldest=UINT32_MAX;
            for(unsigned i=0;i<slots.size();++i)
                if(slots[i].tick<=oldest){oldest=slots[i].tick;target=i;}
        }
        const bool replacing=bool(slots[target].snapshot) && slots[target].tick==tick;
        if(!slots[target].snapshot)++occupied;
        slots[target].snapshot=std::move(owner);slots[target].tick=tick;
        if(!replacing)head=(target+1)%unsigned(slots.size());
    }
    void drop_after(uint32_t tick){for(auto& s:slots)if(s.snapshot && s.tick>tick){s.snapshot.reset();--occupied;}}
    uint32_t oldest() const {if(!occupied)return 0;uint32_t n=UINT32_MAX;for(const auto& s:slots)if(s.snapshot)n=std::min(n,s.tick);return n;}
};
}
#endif
