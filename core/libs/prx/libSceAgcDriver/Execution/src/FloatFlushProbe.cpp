#include "prx/libSceAgcDriver/Execution/include/FloatFlushProbe.hpp"
#include "prx/libSceAgcDriver/Execution/include/FloatProbeTolerance.hpp"
#include "prx/libSceAgcDriver/Execution/include/VulkanDevice.hpp"
#include "prx/libc/include/GuestArena.hpp"

#include <algorithm>
#include <array>
#include <optional>
#include <span>
#include <utility>
#include <stdexcept>
#include <vector>

namespace AgcDriver {
namespace {

using ShaderRecompiler::ShaderStage;

constexpr std::uint32_t Threads = 32;
constexpr std::uint32_t Inputs = 4;
constexpr std::uint32_t Results = 19;
alignas(256) std::array<std::uint32_t, Threads * Inputs> Input{};
alignas(256) std::array<std::uint32_t, Threads * Results> Output{};

alignas(256) constexpr std::array<std::uint32_t, 46> MixedCode{
    0x34020084u, 0x160600ffu, 0x0000004cu, 0xe0381000u, 0x80000401u, 0xbf8c3f70u, 0x06140b04u, 0x08160905u,
    0x10180b04u, 0x0e1a0b04u, 0xd54b000eu, 0x041a0b04u, 0x7e1e0306u, 0x561e0b04u, 0x1e200b04u, 0xd5540011u,
    0x041a0b04u, 0xd5570012u, 0x041a0b04u, 0x7e264904u, 0x7e284505u, 0x7e2a8104u, 0x7c020b04u, 0xd5010016u,
    0x01a90280u, 0x7e2e1b04u, 0x7e307f05u, 0xd5080119u, 0x42020b04u, 0xd55f001au, 0x041a0b04u, 0xd550001bu,
    0x041a0b04u, 0xd550001cu, 0x03ca0b04u, 0xe0781000u, 0x80010a03u, 0xe0781010u, 0x80010e03u, 0xe0781020u,
    0x80011203u, 0xe0781030u, 0x80011603u, 0xe07c1040u, 0x80011a03u, 0xbf810000u,
};

constexpr std::uint32_t MixedRows[Threads][Inputs] = {
    {0x00000001u, 0x3f800000u, 0x00000000u, 0x00000000u},
    {0x80400000u, 0x3f800000u, 0x00800000u, 0x00000000u},
    {0x3f800000u, 0x00012345u, 0x80000001u, 0x00000000u},
    {0x007fffffu, 0x007fffffu, 0x3f800000u, 0x00000000u},
    {0x3f7fffffu, 0x00800000u, 0x00000000u, 0x00000000u},
    {0xbf7fffffu, 0x00800000u, 0x80000000u, 0x00000000u},
    {0x3f7fffffu, 0x00800001u, 0x00000001u, 0x00000000u},
    {0x3f000000u, 0x01000000u, 0x80000001u, 0x00000000u},
    {0x00800000u, 0x80800001u, 0x00800000u, 0x00000000u},
    {0x00800001u, 0x00800000u, 0x3f800000u, 0x00000000u},
    {0x00000000u, 0x80000001u, 0x00000001u, 0x00000000u},
    {0x80000000u, 0x00000001u, 0x807fffffu, 0x00000000u},
    {0x7f800000u, 0x00000001u, 0x3f800000u, 0x00000000u},
    {0x00000001u, 0xff800000u, 0x00000000u, 0x00000000u},
    {0x7fc00000u, 0x00000001u, 0x00000001u, 0x00000000u},
    {0x40000000u, 0x00400000u, 0x80800000u, 0x00000000u},
    {0x3f800000u, 0x3f800000u, 0x80000001u, 0x00000000u},
    {0xbf800000u, 0x00000001u, 0x3f000000u, 0x00000000u},
    {0x34000000u, 0x34000000u, 0x00000000u, 0x00000000u},
    {0x1f800000u, 0x1f800000u, 0x00000000u, 0x00000000u},
    {0x1fffffffu, 0x1fffffffu, 0x00800000u, 0x00000000u},
    {0x20000000u, 0x1f000000u, 0x00000000u, 0x00000000u},
    {0x20000000u, 0x1f7fffffu, 0x00000000u, 0x00000000u},
    {0x20000001u, 0x1f7fffffu, 0x00000000u, 0x00000000u},
    {0xa0000000u, 0x1f7fffffu, 0x80000000u, 0x00000000u},
    {0x3f800000u, 0xbf800000u, 0x00000001u, 0x00000000u},
    {0x00800000u, 0x3f000000u, 0x00000000u, 0x00000000u},
    {0x00800000u, 0x3f7fffffu, 0x00000001u, 0x00000000u},
    {0x41200000u, 0x00000003u, 0x3dcccccdu, 0x00000000u},
    {0xc0490fdbu, 0x80000002u, 0x40490fdbu, 0x00000000u},
    {0x80000001u, 0x80000001u, 0x80000001u, 0x00000000u},
    {0x00000001u, 0x00000001u, 0x00000001u, 0x00000000u}
};

alignas(256) constexpr std::array<std::uint32_t, 30> TinyCode{
    0x34020084u, 0x34040086u, 0xe0381000u, 0x80000401u, 0xbf8c3f70u, 0x10140b04u, 0x0e160b04u, 0xd54b000cu,
    0x041a0b04u, 0x7e1a0306u, 0x561a0b04u, 0x581c0d04u, 0x3f7ffffeu, 0x5a1e0b04u, 0x80000000u, 0xcc200010u,
    0x041a0b04u, 0xd5080011u, 0x22020b04u, 0xd5400012u, 0x041a0b04u, 0x7e260306u, 0x0c260b04u, 0xe0781000u,
    0x80010a02u, 0xe0781010u, 0x80010e02u, 0xe0741020u, 0x80011202u, 0xbf810000u,
};

constexpr std::uint32_t TinyRows[Threads][Inputs] = {
    {0x00800001u, 0x3f7ffffeu, 0x00000000u, 0x00000000u},
    {0x80800001u, 0x3f7ffffeu, 0x80000000u, 0x00000000u},
    {0x3f7ffffeu, 0x00800001u, 0x00000000u, 0x00000000u},
    {0x008003e6u, 0x3f7ff834u, 0x00000000u, 0x00000000u},
    {0x00f80000u, 0x3f042108u, 0x80000000u, 0x00000000u},
    {0x80f80000u, 0x3f042108u, 0x00000000u, 0x00000000u},
    {0x00800f95u, 0x3f7fe0d9u, 0x00000000u, 0x00000000u},
    {0x00800001u, 0x3f7ffffdu, 0x00000000u, 0x00000000u},
    {0x00ffffffu, 0x3f000000u, 0x00000000u, 0x00000000u},
    {0x008005a8u, 0x3f7ff4b0u, 0x00000000u, 0x00000000u},
    {0x00f7ffffu, 0x3f042108u, 0x00000000u, 0x00000000u},
    {0x00ffffffu, 0x3effffffu, 0x00000000u, 0x00000000u},
    {0x008005a9u, 0x3f7ff4aeu, 0x00000000u, 0x00000000u},
    {0x00800001u, 0x3f7ffffeu, 0x00000001u, 0x00000000u},
    {0x007fffffu, 0x3f800001u, 0x00000000u, 0x00000000u},
    {0x00800000u, 0x3f7fffffu, 0x00000000u, 0x00000000u},
    {0x80800001u, 0x3f7ffffeu, 0x80000000u, 0x00000000u},
    {0x00800001u, 0x3f7ffffeu, 0x00000000u, 0x00000000u},
    {0xbf7ffffeu, 0x00800001u, 0x80000000u, 0x00000000u},
    {0x808003e6u, 0x3f7ff834u, 0x80000000u, 0x00000000u},
    {0x80f80000u, 0x3f042108u, 0x00000000u, 0x00000000u},
    {0x00f80000u, 0x3f042108u, 0x80000000u, 0x00000000u},
    {0x80800f95u, 0x3f7fe0d9u, 0x80000000u, 0x00000000u},
    {0x80800001u, 0x3f7ffffdu, 0x80000000u, 0x00000000u},
    {0x80ffffffu, 0x3f000000u, 0x80000000u, 0x00000000u},
    {0x808005a8u, 0x3f7ff4b0u, 0x80000000u, 0x00000000u},
    {0x80f7ffffu, 0x3f042108u, 0x80000000u, 0x00000000u},
    {0x80ffffffu, 0x3effffffu, 0x80000000u, 0x00000000u},
    {0x808005a9u, 0x3f7ff4aeu, 0x80000000u, 0x00000000u},
    {0x80800001u, 0x3f7ffffeu, 0x80000001u, 0x00000000u},
    {0x807fffffu, 0x3f800001u, 0x80000000u, 0x00000000u},
    {0x80800000u, 0x3f7fffffu, 0x80000000u, 0x00000000u}
};

alignas(256) constexpr std::array<std::uint32_t, 28> MixedTinyCode{
    0x34020084u, 0x34040086u, 0xe0381000u, 0x80000401u, 0xbf8c3f70u, 0xcc20000au, 0x141a0b04u, 0xcc20000bu,
    0x0c1a0905u, 0xcc20100cu, 0x141a0b04u, 0xcc20080du, 0x0c1a0905u, 0xcc20400eu, 0x141e0b04u, 0xcc20400fu,
    0x0c1e0905u, 0x7e1002ffu, 0x3f7ffffeu, 0xcc204010u, 0x041e1104u, 0xcc206011u, 0x041e1104u, 0xe0781000u,
    0x80010a02u, 0xe0781010u, 0x80010e02u, 0xbf810000u,
};

constexpr std::uint32_t MixedTinyRows[Threads][Inputs] = {
    {0x00801002u, 0x00003bffu, 0x00000000u, 0x00000000u},
    {0x80801002u, 0x00003bffu, 0x80000000u, 0x80008000u},
    {0x00801000u, 0x3bff3bffu, 0x00000000u, 0x00000000u},
    {0x00801001u, 0x3bff3bffu, 0x00000000u, 0x00000000u},
    {0x00801002u, 0x3bff3bffu, 0x00000000u, 0x00000000u},
    {0x00801003u, 0x3bff3bffu, 0x00000000u, 0x00000000u},
    {0x80801000u, 0x3bff3bffu, 0x80000000u, 0x80008000u},
    {0x80801001u, 0x3bff3bffu, 0x80000000u, 0x80008000u},
    {0x80801002u, 0x3bff3bffu, 0x80000000u, 0x80008000u},
    {0x80801003u, 0x3bff3bffu, 0x80000000u, 0x80008000u},
    {0x00801000u, 0xbbffbbffu, 0x80000000u, 0x80008000u},
    {0x00801001u, 0xbbffbbffu, 0x80000000u, 0x80008000u},
    {0x00801002u, 0xbbffbbffu, 0x80000000u, 0x80008000u},
    {0x00801003u, 0xbbffbbffu, 0x80000000u, 0x80008000u},
    {0x00800001u, 0x3bff3bffu, 0x00000000u, 0x00000000u},
    {0x80800001u, 0x3bff3bffu, 0x80000000u, 0x80008000u},
};

alignas(256) constexpr std::array<std::uint32_t, 18> TranscendentalCode{
    0x34020084u, 0x34040086u, 0xe0381000u, 0x80000401u, 0xbf8c3f70u, 0x7e144f04u, 0x7e166704u, 0x7e185d04u,
    0x7e1a5504u, 0x7e1c4b05u, 0x7e1e4f05u, 0x7e206705u, 0x7e225505u, 0xe0781000u, 0x80010a02u, 0xe0781010u,
    0x80010e02u, 0xbf810000u,
};

constexpr std::uint32_t TranscendentalStride = 16;
constexpr std::array<std::pair<Float32ProbeOp, std::uint32_t>, 8> TranscendentalResults{{
    {Float32ProbeOp::Log2, 0u}, {Float32ProbeOp::Sqrt, 0u}, {Float32ProbeOp::Rsq, 0u}, {Float32ProbeOp::Rcp, 0u},
    {Float32ProbeOp::Exp2, 1u}, {Float32ProbeOp::Log2, 1u}, {Float32ProbeOp::Sqrt, 1u}, {Float32ProbeOp::Rcp, 1u},
}};

constexpr std::uint32_t TranscendentalRows[Threads][Inputs] = {
    {0x40000000u, 0x3f800000u, 0x00000000u, 0x00000000u},
    {0x3f000000u, 0xbf800000u, 0x00000000u, 0x00000000u},
    {0x3f800000u, 0x3f000000u, 0x00000000u, 0x00000000u},
    {0x40400000u, 0xbf000000u, 0x00000000u, 0x00000000u},
    {0x41200000u, 0x41200000u, 0x00000000u, 0x00000000u},
    {0x3dcccccdu, 0xc1200000u, 0x00000000u, 0x00000000u},
    {0x0da24260u, 0x42480000u, 0x00000000u, 0x00000000u},
    {0x7149f2cau, 0xc2480000u, 0x00000000u, 0x00000000u},
    {0x3fc00000u, 0x42c80000u, 0x00000000u, 0x00000000u},
    {0x40e00000u, 0xc2c80000u, 0x00000000u, 0x00000000u},
    {0x3f400000u, 0x40000000u, 0x00000000u, 0x00000000u},
    {0x42c80000u, 0x40400000u, 0x00000000u, 0x00000000u},
    {0x2edbe6ffu, 0x3dcccccdu, 0x00000000u, 0x00000000u},
    {0x501502f9u, 0xbdcccccdu, 0x00000000u, 0x00000000u},
    {0x40490fdbu, 0x40e00000u, 0x00000000u, 0x00000000u},
    {0x402df854u, 0xc0e00000u, 0x00000000u, 0x00000000u},
    {0x3f800001u, 0x3a83126fu, 0x00000000u, 0x00000000u},
    {0x3f7fffffu, 0x41a00000u, 0x00000000u, 0x00000000u},
    {0x0d800000u, 0xc1a00000u, 0x00000000u, 0x00000000u},
    {0x71800000u, 0x3fc00000u, 0x00000000u, 0x00000000u},
    {0x3e99999au, 0xbfc00000u, 0x00000000u, 0x00000000u},
    {0x40a00000u, 0x41f00000u, 0x00000000u, 0x00000000u},
    {0x42800000u, 0xc1f00000u, 0x00000000u, 0x00000000u},
    {0x3c800000u, 0x42800000u, 0x00000000u, 0x00000000u},
    {0x3f8ccccdu, 0x3e800000u, 0x00000000u, 0x00000000u},
    {0x3f666666u, 0x40800000u, 0x00000000u, 0x00000000u},
    {0x60ad78ecu, 0x40a00000u, 0x00000000u, 0x00000000u},
    {0x1e3ce508u, 0x40490fdbu, 0x00000000u, 0x00000000u},
    {0x4640e6b7u, 0x402df854u, 0x00000000u, 0x00000000u},
    {0x3a83126fu, 0x3e99999au, 0x00000000u, 0x00000000u},
    {0x40800000u, 0x3f666666u, 0x00000000u, 0x00000000u},
    {0x3e800000u, 0x3f8ccccdu, 0x00000000u, 0x00000000u}
};

alignas(256) constexpr std::array<std::uint32_t, 47> Float16Code{
    0x34020084u, 0x160600ffu, 0x0000004cu, 0xe0381000u, 0x80000401u, 0xbf8c3f70u, 0xcc0f400au, 0x1a020b04u,
    0xcc10400bu, 0x1a020b04u, 0xcc0e400cu, 0x1c1a0b04u, 0x7e1a0306u, 0x781a0b04u, 0x641c0b04u, 0x6a1e0b04u,
    0xd74b0010u, 0x041a0b04u, 0x7e220306u, 0x6c220b04u, 0xd74b3812u, 0x041a0b04u, 0x7e261507u, 0x7e281704u,
    0x2c100890u, 0x7e2a1708u, 0xcc0e2816u, 0x141a0b04u, 0xcc0f4117u, 0x5a020d04u, 0x7e301705u, 0x7e321506u,
    0xcc10401au, 0x1a020d05u, 0xcc0e401bu, 0x1c120d05u, 0xe0781000u, 0x80010a03u, 0xe0781010u, 0x80010e03u,
    0xe0781020u, 0x80011203u, 0xe0781030u, 0x80011603u, 0xe0741040u, 0x80011a03u, 0xbf810000u,
};

constexpr std::array<std::size_t, 3> Float16WideResults{10u, 11u, 14u};

constexpr std::uint32_t Float16Rows[Threads][Inputs] = {
    {0x80010001u, 0x03ff0001u, 0x80000000u, 0x33000000u},
    {0x3c003c01u, 0x10001000u, 0x80010001u, 0x33c00000u},
    {0x3c013c01u, 0x3c013c01u, 0xbc00bc02u, 0x477ff000u},
    {0xfbff7bffu, 0x7bff7bffu, 0xfbff7bffu, 0x477fe000u},
    {0x00007c00u, 0xfc000000u, 0x7e003c00u, 0x7fc00001u},
    {0x7d017e00u, 0x3c00fe55u, 0x00017c01u, 0xff800000u},
    {0x80008000u, 0x00008000u, 0x00008000u, 0x80000000u},
    {0x04010400u, 0x38013800u, 0x00008001u, 0x38800000u},
    {0x24000200u, 0x14000200u, 0x80010001u, 0x387fe000u},
    {0x42485640u, 0xc2485640u, 0x4890f0e0u, 0x33800001u},
    {0xb5553555u, 0x42003c00u, 0x35550000u, 0x3eaaaaabu},
    {0x78007800u, 0x38004000u, 0x7bfffbffu, 0x47800000u},
    {0x840003ffu, 0x3c003c00u, 0x00010001u, 0x387fc000u},
    {0x2e661001u, 0x2e661001u, 0x80018000u, 0xc77fefffu},
    {0xc47c00dcu, 0x00000000u, 0x7f30b95eu, 0x4502c2fdu},
    {0xf88ecb12u, 0xe21c8000u, 0xfadcfdf9u, 0x9fdf0574u},
    {0xb7917c06u, 0xfbe9576eu, 0x75030099u, 0x469faa6du},
    {0x18d78dc1u, 0x00003fceu, 0x60000117u, 0xc5691b21u},
    {0xc4b9ba98u, 0xfefc7c00u, 0x7c0081a8u, 0xb4bb1bd8u},
    {0xe35581cbu, 0x7c007bfau, 0x7e693669u, 0xe923e53au},
    {0x14cffd79u, 0x8ab7ba3cu, 0x02187f62u, 0xb36dfc0cu},
    {0x03a38000u, 0x762b0c3du, 0xf7b38000u, 0xd6313affu},
    {0x1cf83a09u, 0xb655146bu, 0xffac7c00u, 0x33ded778u},
    {0xb42d0133u, 0x0000c68fu, 0x7c00007fu, 0x75545595u},
    {0x8151eed3u, 0xc664fa49u, 0x5b064dd1u, 0xb259b46bu},
    {0x0dfb7eb4u, 0x8354747bu, 0x3fdd8000u, 0xc5fbd33du},
    {0x82977daau, 0x80003a59u, 0x40537d86u, 0xc5f7e4bbu},
    {0xfc000b87u, 0x87c48262u, 0x09972ce7u, 0x45e89b65u},
    {0xad3c7f13u, 0x9df3f340u, 0x30974fa2u, 0x2a8cd90cu},
    {0x81b9405cu, 0x7f3fc62au, 0x7909809fu, 0x8fb4c2b9u},
    {0xf9a00113u, 0x30cef570u, 0xfc0076cau, 0xe2586cbcu},
    {0x00007c68u, 0xd45a8f28u, 0xfb73fc66u, 0xa94893e7u},
};

#ifdef ANYPS5_TRANSLATED_GUEST
constexpr std::size_t ProbePageBytes = 16384;
constexpr std::size_t ProbeInputOffset = 256;
constexpr std::size_t ProbeOutputOffset = 1024;
static_assert(ProbeOutputOffset + Threads * Results * sizeof(std::uint32_t) <= ProbePageBytes);

std::uint8_t* ProbePage() {
    static auto* const page = [] {
        auto* bytes = static_cast<std::uint8_t*>(GuestArena::GuestArenaAllocate_nid_postfix(ProbePageBytes, ProbePageBytes));
        if (bytes == nullptr) throw std::runtime_error("AGC driver: the guest arena has no page for the f32 denormal flush probe");
        GuestArena::GuestArenaCommit_nid_postfix(bytes, ProbePageBytes, 0x04, ProbePageBytes);
        return bytes;
    }();
    return page;
}
#endif

std::array<std::uint32_t, 4> BufferDescriptor(const void* data, std::uint32_t bytes) {
    const auto address = GuestArena::HostToGuest(data);
    return {static_cast<std::uint32_t>(address), static_cast<std::uint32_t>((address >> 32u) & 0xffffu), bytes, 0x31016facu};
}

bool IsNan(std::uint32_t value) {
    return (value & 0x7fffffffu) > 0x7f800000u;
}

std::vector<std::uint32_t> Run(VulkanDevice& device, const ShaderRecompiler::SpirvTarget& target, const std::optional<ShaderRecompiler::ShaderFloatMode>& floatMode, std::span<const std::uint32_t> code, const std::uint32_t (*rows)[Inputs]) {
#ifdef ANYPS5_TRANSLATED_GUEST
    auto* const page = ProbePage();
    auto* const program = reinterpret_cast<std::uint32_t*>(page);
    std::copy(code.begin(), code.end(), program);
    code = {program, code.size()};
    const std::span<std::uint32_t, Threads * Inputs> inputs{reinterpret_cast<std::uint32_t*>(page + ProbeInputOffset), Threads * Inputs};
    const std::span<std::uint32_t, Threads * Results> outputs{reinterpret_cast<std::uint32_t*>(page + ProbeOutputOffset), Threads * Results};
#else
    const std::span<std::uint32_t, Threads * Inputs> inputs{Input};
    const std::span<std::uint32_t, Threads * Results> outputs{Output};
#endif
    for (std::uint32_t tid = 0; tid < Threads; ++tid) std::copy(std::begin(rows[tid]), std::end(rows[tid]), &inputs[tid * Inputs]);
    std::fill(outputs.begin(), outputs.end(), 0xdeadbeefu);
    std::vector<std::uint32_t> userData(8, 0u);
    const auto input = BufferDescriptor(inputs.data(), static_cast<std::uint32_t>(inputs.size() * 4u));
    const auto output = BufferDescriptor(outputs.data(), static_cast<std::uint32_t>(outputs.size() * 4u));
    std::copy(input.begin(), input.end(), userData.begin());
    std::copy(output.begin(), output.end(), userData.begin() + 4);
    const auto codeAddress = GuestArena::HostToGuest(code.data());
    const std::array<ShaderRecompiler::MemoryRegion, 1> memory{{{codeAddress, std::as_bytes(code)}}};
    const ShaderRecompiler::ShaderComputeStageInfo compute{{Threads, 1, 1}, 0u, {false, false, false}, false, 1};
    ShaderRecompiler::RecompileRequest request{
        {ShaderStage::Compute, codeAddress, code, 0, {}},
        {32, 0, userData, compute, std::nullopt, std::nullopt, memory},
        target,
        {0, 0, 0, 128}
    };
    request.useCache = false;
    request.context.floatMode = floatMode;
    const auto result = ShaderRecompiler::Recompile(request);
    device.Dispatch(result, 1, 1, 1, {}, codeAddress);
    device.WaitIdle();
    return {outputs.begin(), outputs.end()};
}

void Compare(FloatFlushProbeResult& result, const std::vector<std::uint32_t>& expected, const std::vector<std::uint32_t>& actual) {
    for (std::size_t index = 0; index < expected.size(); ++index) {
        ++result.compared;
        if (expected[index] != actual[index] && !(IsNan(expected[index]) && IsNan(actual[index]))) ++result.mismatches;
    }
}

void CompareTranscendental(FloatFlushProbeResult& result, const std::vector<std::uint32_t>& expected, const std::vector<std::uint32_t>& actual) {
    for (std::uint32_t tid = 0; tid < Threads; ++tid) {
        for (std::uint32_t slot = 0; slot < TranscendentalResults.size(); ++slot) {
            const auto [op, input] = TranscendentalResults[slot];
            const auto index = tid * TranscendentalStride + slot;
            ++result.compared;
            if (!Float32ResultsAgree(op, TranscendentalRows[tid][input], expected[index], actual[index])) ++result.mismatches;
        }
    }
}

}

FloatFlushProbeResult ProbeFloat32FlushToZero(VulkanDevice& device, const ShaderRecompiler::SpirvTarget& target) {
    FloatFlushProbeResult result;
    const std::array<std::optional<ShaderRecompiler::ShaderFloatMode>, 3> modes{std::nullopt, ShaderRecompiler::ShaderFloatMode{0xc0u, true, false, false}, ShaderRecompiler::ShaderFloatMode{0x00u, false, true, false}};
    const std::array<std::pair<std::span<const std::uint32_t>, const std::uint32_t (*)[Inputs]>, 3> programs{{{MixedCode, MixedRows}, {TinyCode, TinyRows}, {MixedTinyCode, MixedTinyRows}}};
    for (const auto& mode : modes) {
        for (const auto& [code, rows] : programs) {
            auto software = target;
            software.float32FlushToZero = false;
            auto hardware = target;
            hardware.float32FlushToZero = true;
            Compare(result, Run(device, software, mode, code, rows), Run(device, hardware, mode, code, rows));
        }
    }
    return result;
}

FloatFlushProbeResult ProbeFloat32DenormPreserve(VulkanDevice& device, const ShaderRecompiler::SpirvTarget& target) {
    FloatFlushProbeResult result;
    auto preserved = target;
    preserved.float32FlushToZero = false;
    preserved.float32DenormPreserve = true;
    auto unpreserved = preserved;
    unpreserved.float32DenormPreserve = false;
    CompareTranscendental(result, Run(device, unpreserved, std::nullopt, TranscendentalCode, TranscendentalRows), Run(device, preserved, std::nullopt, TranscendentalCode, TranscendentalRows));
    return result;
}

FloatFlushProbeResult ProbeFloat16Arithmetic(VulkanDevice& device, const ShaderRecompiler::SpirvTarget& target) {
    FloatFlushProbeResult result;
    auto emulated = target;
    emulated.float16Measured = false;
    auto native = target;
    native.float16Measured = true;
    const auto sameHalf = [](std::uint32_t left, std::uint32_t right) { return left == right || ((left & 0x7fffu) > 0x7c00u && (right & 0x7fffu) > 0x7c00u); };
    const std::array<std::optional<ShaderRecompiler::ShaderFloatMode>, 3> modes{std::nullopt, ShaderRecompiler::ShaderFloatMode{0xc0u, false, true, false}, ShaderRecompiler::ShaderFloatMode{0xc0u, true, false, true}};
    for (const auto& mode : modes) {
        const auto expected = Run(device, emulated, mode, Float16Code, Float16Rows);
        const auto actual = Run(device, native, mode, Float16Code, Float16Rows);
        for (std::size_t index = 0; index < expected.size(); ++index) {
            ++result.compared;
            const bool wide = std::ranges::find(Float16WideResults, index % Results) != Float16WideResults.end();
            const bool same = wide ? expected[index] == actual[index] || (IsNan(expected[index]) && IsNan(actual[index]))
                                   : sameHalf(expected[index] & 0xffffu, actual[index] & 0xffffu) && sameHalf(expected[index] >> 16u, actual[index] >> 16u);
            if (!same) ++result.mismatches;
        }
    }
    return result;
}

}
