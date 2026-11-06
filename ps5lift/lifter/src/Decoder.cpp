#include <lifter/Decoder.hpp>

#include <stdexcept>

namespace Ps5Lift {

void InitializeDecoder(ZydisDecoder& decoder) {
    if (ZYAN_FAILED(ZydisDecoderInit(&decoder, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_STACK_WIDTH_64)) ||
        ZYAN_FAILED(ZydisDecoderEnableMode(&decoder, ZYDIS_DECODER_MODE_AMD_BRANCHES, ZYAN_TRUE))) {
        throw std::runtime_error("Zydis decoder initialization failed");
    }
}

}
