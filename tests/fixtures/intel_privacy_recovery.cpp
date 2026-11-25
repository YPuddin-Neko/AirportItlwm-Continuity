// Appended to the native-call fixture's shared synthetic ABI setup.
static void coldUnavailableController() {
    reset();
    intelPrivacySetEnabled(0);
    free(intelPrivacyRuntime.state);
    intelPrivacyRuntime = {};
    intelController = false;
    intelPrivacySetEnabled(1);
}

int main() {
    unsigned cases = 0;
    coldUnavailableController();
    observe();
    assert(admit() == nativeValue);
    // A transient busy gate at startup must recover without another wake.
    intelController = true;
    observe();
    assert(admit() == 0x016873e4ce478aULL);
    ++cases;

    coldUnavailableController();
    for (unsigned i = 0; i != 16; ++i) {
        ++ticks;
        report[7] = static_cast<uint8_t>(i);
        observe();
        assert(admit() == nativeValue);
    }
    intelController = true;
    assert(admit() == nativeValue);
    report[7] = 0x99;
    observe();
    assert(admit() == 0x016873e4ce4799ULL);
    ++cases;

    reset(); observe();
    assert(admit() == 0x016873e4ce478aULL);
    intelPrivacySetEnabled(0);
    intelController = false;
    intelPrivacySetEnabled(1);
    observe();
    assert(admit() == nativeValue);
    intelController = true;
    assert(admit() == nativeValue);
    observe();
    assert(admit() == 0x016873e4ce478aULL);
    ++cases;

    coldUnavailableController();
    intelPrivacySetEnabled(0);
    intelController = true;
    observe();
    assert(admit() == nativeValue);
    intelPrivacySetEnabled(1);
    observe();
    assert(admit() == 0x016873e4ce478aULL);
    ++cases;

    intelPrivacySetEnabled(0);
    free(intelPrivacyRuntime.state);
    intelPrivacyRuntime.state = nullptr;
    printf("IntelPrivacy recovery: %u lifecycle cases passed\n", cases);
}
