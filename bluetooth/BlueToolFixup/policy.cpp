using u32 = unsigned;
extern "C" __attribute__((noinline,used)) bool intelStandardScan(void *controller) {
    u32 chip = *reinterpret_cast<u32 *>(static_cast<unsigned char *>(controller) + 0x2f8);
    return chip <= 1 || (chip >= 19 && chip <= 25) || (chip >= 2001 && chip <= 3999) || (chip >= 5000 && chip <= 6000);
}
extern "C" __attribute__((noinline,used)) bool intelStandardSets(void *controller) {
    u32 chip = *reinterpret_cast<u32 *>(static_cast<unsigned char *>(controller) + 0x2f8);
    return chip <= 1 || (chip >= 19 && chip <= 25) || (chip >= 2001 && chip <= 3999) || chip >= 5000;
}
