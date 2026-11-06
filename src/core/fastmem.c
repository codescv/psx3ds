#include "psx3ds/bus.h"

void fastmem_build(PsxBus *b) {
    (void)b;
}

void fastmem_set_isolated(PsxBus *b, bool isc) {
    (void)b;
    (void)isc;
}

void bus_code_page_ref(PsxBus *b, u32 phys_page) {
    (void)b;
    (void)phys_page;
}

void bus_code_page_unref(PsxBus *b, u32 phys_page) {
    (void)b;
    (void)phys_page;
}
