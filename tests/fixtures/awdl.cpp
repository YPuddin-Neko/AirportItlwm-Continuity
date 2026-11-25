#include <cassert>
#include <cstring>
#include <vector>
#include "AirportItlwm/AWDL/ActionFrame.hpp"

using namespace AirportAWDL;
using Bytes = std::vector<uint8_t>;

// Synthetic wire examples, not captures from the target phone.
static Bytes hex(const char *s) {
    Bytes b;
    int high = -1;
    for (; *s; ++s) {
        int n = *s >= '0' && *s <= '9' ? *s - '0' :
            (*s >= 'a' && *s <= 'f' ? *s - 'a' + 10 : -1);
        if (n < 0) continue;
        if (high < 0) high = n;
        else { b.push_back((high << 4) | n); high = -1; }
    }
    assert(high < 0);
    return b;
}

static Bytes action(uint8_t subtype = 0) {
    auto b = hex("7f0017f2 08100000 78563412 f0debc9a");
    b[6] = subtype;
    return b;
}

static void append(Bytes &b, uint8_t type, const Bytes &value) {
    b.push_back(type);
    b.push_back(value.size() & 255);
    b.push_back(value.size() >> 8);
    b.insert(b.end(), value.begin(), value.end());
}

static Bytes sync() {
    return hex("06 1000 2c 00 1000 6e00 0018 1000 1000 0000 "
               "03 03 03 03 021122334455 04 00 3412 0000");
}

static Bytes sequence() {
    return hex("0f030003ffff "
               "0651 2c80 9580 0651 2c80 9580 0651 2c80 "
               "9580 0651 2c80 9580 0651 2c80 9580 0651");
}

static Bytes v1() {
    return hex("00 3412 02 00 02aabbccddee 07000000 09000000 0000");
}

static Bytes v2() {
    return hex("021122334455 0266778899aa 78563412 03000000 "
               "efcdab90 40302010 78563412 00000000 ffffffff");
}

static Bytes complete() {
    auto b = action(3);
    append(b, 4, sync());
    append(b, 5, v1());
    append(b, 18, sequence());
    append(b, 24, v2());
    append(b, 21, hex("31 02"));
    return b;
}

static Bytes mpdu(const Bytes &body) {
    auto b = hex("d000 0000 ffffffffffff 021122334455 002500ff9473 3000");
    assert(b.size() == 24);
    b.insert(b.end(), body.begin(), body.end());
    return b;
}

static void fields() {
    Advertisement a = {};
    auto b = complete();
    assert(parseAction(b.data(), b.size(), a) == ParseResult::Valid);
    assert(a.fields == (Sync | Channels | ElectionV1 | ElectionV2 | Version));
    assert(a.subtype == 3 && a.version == 0x31 && a.deviceClass == 2);
    assert(a.phyTxTime == 0x12345678 && a.targetTxTime == 0x9abcdef0);
    assert(a.nextWindowTU == 16 && a.windowPeriodTU == 16 && a.actionPeriodTU == 110);
    assert(a.windowCounter == 0x1234 && a.presenceMode == 4 && a.masterChannel == 44);
    assert(a.syncMaster[0] == 2 && a.syncMaster[5] == 0x55);
    assert(a.masterCounter == 0x12345678 && a.distance == 3);
    assert(a.masterMetric == 0x90abcdef && a.selfMetric == 0x10203040);
    assert(a.selfCounter == 0xffffffff && a.electionSync[5] == 0xaa);
    assert(a.electionID == 0x12345678);
    assert(a.sequence.count == 16 && a.sequence.encoding == 3);
    assert(a.sequence.fill == 0xffff && a.sequence.step == 3);
    assert(a.sequence.channel[1] == 44 && a.sequence.channel[2] == 149);
    assert(a.sequence.flags[0] == 0x51 && a.sequence.flags[2] == 0x80);

    // A later v1 TLV must not overwrite v2 election information.
    append(b, 5, v1());
    append(b, 0xfe, hex("aabbcc"));
    append(b, 0xfd, {});
    assert(parseAction(b.data(), b.size(), a) == ParseResult::Valid);
    assert(a.distance == 3 && a.selfMetric == 0x10203040 && a.electionID == 0x12345678);
    auto legacy = action(); append(legacy, 5, v1());
    assert(parseAction(legacy.data(), legacy.size(), a) == ParseResult::Valid);
    assert(a.electionID == 0x1234);

    for (uint8_t enc : {0, 1, 3}) {
        auto s = hex("01000003ffff");
        s[1] = enc;
        if (enc == 0) { s.push_back(6); s.push_back(44); }
        if (enc == 1) { auto tail=hex("9006 802c"); s.insert(s.end(),tail.begin(),tail.end()); }
        if (enc == 3) { auto tail=hex("0651 2c80"); s.insert(s.end(),tail.begin(),tail.end()); }
        auto packet = action();
        append(packet, 18, s);
        assert(parseAction(packet.data(), packet.size(), a) == ParseResult::Valid);
        assert(a.sequence.count == 2 && a.sequence.channel[0] == 6 && a.sequence.channel[1] == 44);
        assert(a.sequence.flags[0] == (enc == 0 ? 0 : enc == 1 ? 0x90 : 0x51));
    }

    auto embedded = sync();
    auto s = sequence();
    embedded.insert(embedded.end(), s.begin(), s.end());
    embedded.push_back(0); embedded.push_back(0);
    b = action();
    append(b, 4, embedded);
    assert(parseAction(b.data(), b.size(), a) == ParseResult::Valid);
    assert(a.sequence.channel[2] == 149);
    auto explicitS = hex("00000003ffff 2c");
    b = action(); append(b, 18, explicitS); append(b, 4, embedded);
    assert(parseAction(b.data(), b.size(), a) == ParseResult::Valid);
    assert(a.sequence.count == 1 && a.sequence.channel[0] == 44);
}

static void rejected(const Bytes &b, ParseResult expected) {
    Advertisement a;
    std::memset(&a, 0x5a, sizeof(a));
    Advertisement before = a;
    assert(parseAction(b.data(), b.size(), a) == expected);
    assert(std::memcmp(&a, &before, sizeof(a)) == 0);
}

static void invalid() {
    auto b = action(); b[3] = 0xf1; rejected(b, ParseResult::Other);
    b = action(); b.resize(15); rejected(b, ParseResult::Malformed);
    b = action(); b[5] = 0x20; rejected(b, ParseResult::Unsupported);
    b = action(1); rejected(b, ParseResult::Unsupported);
    b = action(); b.push_back(21); rejected(b, ParseResult::Malformed);
    b = action(); append(b,21,hex("31")); rejected(b,ParseResult::Malformed);
    b = complete(); b.pop_back(); rejected(b,ParseResult::Malformed);
    b = action(); auto s=sequence(); s[1]=2; append(b,18,s); rejected(b,ParseResult::Unsupported);
    b = action(); s=sequence(); s[0]=255; append(b,18,s); rejected(b,ParseResult::Malformed);
    b = action(); s=Bytes(6+17, 6); s[0]=16; s[1]=0; append(b,18,s); rejected(b,ParseResult::Unsupported);
    b = action(); s=sync(); s[5]=0; append(b,4,s); rejected(b,ParseResult::Malformed);
    b = action(); s=sync(); s[27]=0; append(b,4,s); rejected(b,ParseResult::Malformed);
    b = action(); s=v2(); s.resize(39); append(b,24,s); rejected(b,ParseResult::Malformed);
}

static void frames() {
    Advertisement a = {};
    auto b = mpdu(complete());
    assert(parseMPDU(b.data(), b.size(), a) == ParseResult::Valid);
    auto ht = b; ht[1] = 0x80; ht.insert(ht.begin()+24, 4, 0);
    assert(parseMPDU(ht.data(), ht.size(), a) == ParseResult::Valid);
    for (uint8_t flags : {1, 2, 3, 4}) {
        auto wrong = b; wrong[1] = flags;
        assert(parseMPDU(wrong.data(),wrong.size(),a) == ParseResult::Malformed);
    }
    auto wrong = b; wrong[22] |= 1;
    assert(parseMPDU(wrong.data(),wrong.size(),a) == ParseResult::Malformed);
    wrong=b; wrong[10]|=1;
    assert(parseMPDU(wrong.data(),wrong.size(),a) == ParseResult::Malformed);
    wrong=b; wrong[1]=0x40;
    assert(parseMPDU(wrong.data(),wrong.size(),a) == ParseResult::Other);
    wrong=b; wrong[0]=0x80;
    assert(parseMPDU(wrong.data(),wrong.size(),a) == ParseResult::Other);

    ReceiveState state = {};
    assert(state.receive(b.data(),b.size(),44,-48));
    assert(state.valid==1 && state.mif==1 && state.psf==0);
    assert(state.lastRSSI==-48 && state.lastChannel==44 && state.lastSender[5]==0x55);
    auto last=state.last;
    wrong=b; wrong.pop_back();
    assert(state.receive(wrong.data(),wrong.size(),6,-70));
    assert(state.valid==1 && state.malformed==1 && state.lastChannel==44);
    assert(std::memcmp(&state.last,&last,sizeof(last))==0);
    wrong=b; wrong[29]=0x20;
    assert(state.receive(wrong.data(),wrong.size(),6,-70));
    assert(state.unsupported==1 && state.lastRSSI==-48);
    wrong=b; wrong[0]=0x80;
    assert(!state.receive(wrong.data(),wrong.size(),6,-70));
    b=mpdu(action());
    assert(state.receive(b.data(),b.size(),149,-60));
    assert(state.psf==1 && state.mif==1 && state.valid==2);
}

static void mutations() {
    Advertisement a = {};
    assert(parseAction(nullptr,0,a)==ParseResult::Other);
    assert(parseMPDU(nullptr,0,a)==ParseResult::Other);
    auto base=mpdu(complete());
    for(size_t length=0;length<=base.size();++length) {
        // Exact allocations let ASan detect reads beyond a truncated frame.
        auto b=Bytes(base.begin(),base.begin()+length);
        parseMPDU(b.data(),b.size(),a);
        auto body=complete(); body.resize(length < body.size() ? length : body.size());
        parseAction(body.data(),body.size(),a);
    }
    for(unsigned shift=1;shift<=7;++shift) {
        Bytes b(shift,0); b.insert(b.end(),base.begin(),base.end());
        assert(parseMPDU(b.data()+shift,base.size(),a)==ParseResult::Valid);
        assert(a.masterMetric==0x90abcdef);
    }
    uint32_t random=0x9462;
    for(unsigned i=0;i<50000;++i) {
        Bytes b=base;
        for(unsigned n=0;n<4;++n) {
            random=random*1664525U+1013904223U;
            b[random%b.size()]=random>>24;
        }
        Advertisement before=a;
        auto result=parseMPDU(b.data(),b.size(),a);
        if(result!=ParseResult::Valid)
            assert(std::memcmp(&a,&before,sizeof(a))==0);
    }
}

static void htCapabilities() {
    for (unsigned length : {0u,2u,3u,5u,6u,7u,8u,28u,29u}) {
        auto b=action();Bytes value(length,0);
        if (length>=6) value[5]=0xa5;
        if (length>=7) value[6]=0x5a;
        append(b,7,value);
        Advertisement a{}, before{};a.fields=Version;a.version=0x31;before=a;
        const auto result=parseAction(b.data(),b.size(),a);
        if (length<3 || length>28) {
            assert(result==ParseResult::Malformed && !memcmp(&a,&before,sizeof(a)));
        } else {
            assert(result==ParseResult::Valid && a.fields==HTCapabilities);
            assert(a.rxHTMcs==(length>=6?0xa5:0));
            assert(a.rxHTMcs2==(length>=7?0x5a:0));
        }
    }
    auto b=complete();append(b,7,hex("00 00 6e01 1b ff 00 00"));
    Advertisement a{};assert(parseAction(b.data(),b.size(),a)==ParseResult::Valid);
    assert(a.rxHTMcs==0xff && a.fields==63 && a.version==0x31 && a.sequence.count==16);
    auto before=a;b.pop_back();
    assert(parseAction(b.data(),b.size(),a)==ParseResult::Malformed && !memcmp(&a,&before,sizeof(a)));
}

int main() {
    if (AWDL_TEST_CASE==1) fields();
    if (AWDL_TEST_CASE==2) invalid();
    if (AWDL_TEST_CASE==3) frames();
    if (AWDL_TEST_CASE==4) mutations();
    if (AWDL_TEST_CASE==5) htCapabilities();
}
