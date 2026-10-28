#include "snapshot_pages.h"
#include "retcomm_rbengine/snap_ring.h"
#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>

using namespace vb_history;
static Snapshot capture(PagePool& pool,const std::vector<uint8_t>& data) {
    Builder builder(pool);size_t offset=0;
    for(size_t n:{size_t(7),size_t(19),size_t(5001),size_t(3)}) {
        n=std::min(n,data.size()-offset);builder.append(data.data()+offset,n);offset+=n;
    }
    builder.append(data.data()+offset,data.size()-offset);return builder.finish();
}
static void verify(const Snapshot& s,const std::vector<uint8_t>& data) {
    assert(s.size==data.size());std::vector<uint8_t> restored(data.size());
    assert(s.read(0,restored.data(),restored.size()));assert(restored==data);
    for(size_t at=0;at<data.size();at+=997) {
        const size_t n=std::min(size_t(7301),data.size()-at);
        std::vector<uint8_t> span(n);assert(s.read(at,span.data(),n));
        assert(!std::memcmp(span.data(),data.data()+at,n));
    }
    assert(!s.read(s.size+1,nullptr,0));assert(!s.read(0,nullptr,1));
    assert(s.read(s.size,nullptr,0));
}
static void contracts() {
    PagePool pool(8);std::vector<uint8_t> original(27001);
    for(size_t i=0;i<original.size();++i)original[i]=uint8_t(i*37);
    Snapshot first=capture(pool,original),same=capture(pool,original);
    verify(first,original);verify(same,original);assert(pool.costs()->hits>0);
    auto changed=original;changed[0]^=2;changed[17000]^=4;
    Snapshot second=capture(pool,changed);verify(first,original);verify(second,changed);
    // Force sample-fingerprint collisions at unobserved positions. Exact
    // equality must reject them and bounded eviction must preserve owners.
    std::vector<uint8_t> colliding(page_size);
    std::vector<PageRef> held;
    const auto fingerprint=PagePool::fingerprint(colliding.data(),colliding.size());
    for(unsigned i=0;i<12;++i) {
        colliding[111]=uint8_t(i);
        assert(PagePool::fingerprint(colliding.data(),colliding.size())==fingerprint);
        held.push_back(pool.intern(colliding.data(),colliding.size()));
    }
    for(unsigned i=0;i<12;++i)assert(held[i]->bytes[111]==i);
    assert(pool.indexed_buckets()<=8);
    auto costs=pool.costs();first={};same={};second={};held.clear();
    assert(costs->live_pages==0); // weak lookup entries do not keep pages alive
    for(unsigned bad:{0u,513u,UINT32_MAX}) {
        bool rejected=false;try{Ring invalid(bad);}catch(const std::invalid_argument&){rejected=true;}
        assert(rejected);
    }
    Ring ring(4);auto* reference=rbe_snap_ring_create(4);assert(reference);
    for(uint32_t tick:{90u,10u,50u,20u,10u,70u,0u,UINT32_MAX}) {
        std::vector<uint8_t> bytes(9001,uint8_t(tick));
        ring.store(tick,capture(ring.pool,bytes));
        auto* copy=static_cast<uint8_t*>(std::malloc(bytes.size()));assert(copy);
        std::memcpy(copy,bytes.data(),bytes.size());assert(rbe_snap_ring_store(reference,tick,copy,bytes.size()));
        assert(ring.count()==rbe_snap_ring_count(reference));assert(ring.oldest()==rbe_snap_ring_oldest_tick(reference));
        for(uint32_t key:{0u,10u,20u,50u,70u,90u,UINT32_MAX}) {
            size_t n=0;const auto* old=rbe_snap_ring_peek(reference,key,&n);const auto* now=ring.peek(key);
            assert(bool(old)==bool(now));if(now){std::vector<uint8_t> expected(old,old+n);verify(*now,expected);}
        }
    }
    ring.drop_after(50);rbe_snap_ring_drop_after(reference,50);
    assert(ring.count()==rbe_snap_ring_count(reference));assert(ring.oldest()==rbe_snap_ring_oldest_tick(reference));
    ring.clear();rbe_snap_ring_clear(reference);assert(!ring.count());assert(ring.pool.costs()->live_pages==0);
    uint32_t random=0x12345678;
    for(unsigned operation=0;operation<1000;++operation) {
        random=random*1664525u+1013904223u;const uint32_t tick=(random>>12)%37;
        if((random&15)==0){ring.clear();rbe_snap_ring_clear(reference);}
        else if((random&7)==1){ring.drop_after(tick);rbe_snap_ring_drop_after(reference,tick);}
        else {
            std::vector<uint8_t> bytes(109,uint8_t(operation));
            ring.store(tick,capture(ring.pool,bytes));
            auto* copy=static_cast<uint8_t*>(std::malloc(bytes.size()));assert(copy);
            std::memcpy(copy,bytes.data(),bytes.size());assert(rbe_snap_ring_store(reference,tick,copy,bytes.size()));
        }
        assert(ring.count()==rbe_snap_ring_count(reference));assert(ring.oldest()==rbe_snap_ring_oldest_tick(reference));
        for(uint32_t key=0;key<37;++key) {
            size_t n=0;const auto* old=rbe_snap_ring_peek(reference,key,&n);const auto* now=ring.peek(key);
            assert(bool(old)==bool(now));if(now)verify(*now,std::vector<uint8_t>(old,old+n));
        }
    }
    rbe_snap_ring_destroy(reference);
}
static void versioned_contracts() {
    PagePool pool(8);std::array<uint8_t,page_size> bytes{};
    auto a=pool.intern_versioned(bytes.data(),bytes.size(),1);
    auto b=pool.intern_versioned(bytes.data(),bytes.size(),1);assert(a==b);
    assert(pool.costs()->version_hits==1);
    bytes[111]=7;auto c=pool.intern_versioned(bytes.data(),bytes.size(),2);
    assert(c!=a && a->bytes[111]==0 && c->bytes[111]==7);
    bytes[111]=9;auto mutable_page=pool.intern_versioned(bytes.data(),bytes.size(),0);
    assert(mutable_page->bytes[111]==9 && c->bytes[111]==7);
    std::array<std::array<uint8_t,page_size>,16> sources{};
    std::vector<PageRef> held;
    for(unsigned i=0;i<sources.size();++i){sources[i].fill(uint8_t(i));held.push_back(pool.intern_versioned(sources[i].data(),page_size,3));}
    assert(pool.indexed_versions()<=8);
    for(unsigned i=0;i<sources.size();++i)assert(held[i]->bytes[0]==i);
    auto cost=pool.costs();a={};b={};c={};mutable_page={};held.clear();assert(cost->live_pages==0);
    // An expired weak cache entry cannot retain data or defeat fresh ownership.
    bytes[111]=11;auto fresh=pool.intern_versioned(bytes.data(),page_size,2);assert(fresh->bytes[111]==11);fresh={};
    bytes[111]=13;fresh=pool.intern_versioned(bytes.data(),page_size,2);assert(fresh->bytes[111]==13);fresh={};
    uint64_t token=10;uint32_t random=0x810;
    std::vector<uint8_t> prefix(19,0x5a),suffix(31,0xa5),expected;
    std::vector<Snapshot> history;std::vector<std::vector<uint8_t>> copies;
    for(unsigned sample=0;sample<1000;++sample){
        random=random*1664525u+1013904223u;
        if(sample%3==0){bytes[random%page_size]^=uint8_t(random>>24);++token;}
        Builder builder(pool);builder.append(prefix.data(),prefix.size());
        builder.append_versioned(bytes.data(),bytes.size(),token);builder.append(suffix.data(),suffix.size());
        expected=prefix;expected.insert(expected.end(),bytes.begin(),bytes.end());expected.insert(expected.end(),suffix.begin(),suffix.end());
        history.push_back(builder.finish());copies.push_back(expected);verify(history.back(),expected);
        if(history.size()>17){history.erase(history.begin());copies.erase(copies.begin());}
        for(unsigned i=0;i<history.size();++i)verify(history[i],copies[i]);
    }
    history.clear();assert(cost->live_pages==0);
}
int main(int argc,char** argv) {
    contracts();versioned_contracts();PagePool pool;std::vector<Snapshot> snapshots;uint64_t logical=0;
    for(int i=1;i<argc;++i) {
        std::ifstream f(std::filesystem::u8path(argv[i]),std::ios::binary);assert(f);
        std::vector<uint8_t> data((std::istreambuf_iterator<char>(f)),{});assert(!data.empty());
        snapshots.push_back(capture(pool,data));verify(snapshots.back(),data);logical+=data.size();
    }
    auto costs=pool.costs();
    std::cout<<"{\"passed\":true,\"snapshots\":"<<snapshots.size()<<",\"logical_bytes\":"<<logical
             <<",\"allocated_pages\":"<<costs->allocated_pages<<",\"page_data_bytes_copied\":"<<costs->copied_bytes
             <<",\"page_hits\":"<<costs->hits<<",\"live_page_storage_bytes\":"<<costs->live_pages*sizeof(Page)<<"}\n";
    snapshots.clear();assert(costs->live_pages==0);
}
