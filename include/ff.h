/* daisycola: FatFs's ff.h, with f_size() evaluating the way it does on the STM32.
 *
 * TAPE computes sample start and end points as `val * (f_size(fp) - sizeof(header))`, assigned to
 * a uint32_t, and it does this before the file is open, when f_size() is 0. On the STM32 size_t
 * is 32 bits, so the subtraction wraps to 2^32 - 44, the product rounds to 2^32, and GCC-ARM's
 * float-to-u32 conversion (vcvt) saturates it to 0xFFFFFFFF: the end point looks huge and is
 * accepted. On x86-64 size_t is 64 bits and the conversion wraps, so the end point comes out as 0
 * and is rejected, which leaves a voice's first play window empty.
 *
 * In C++, f_size() returns a ChipSize instead of FatFs's DWORD. Arithmetic on it wraps at 32 bits,
 * and multiplying it by a float or double gives a ChipReal, which converts to uint32_t with the
 * chip's saturation. With a file open every value is in range, so nothing changes. ff.c is C and
 * keeps the original macro.
 */
#pragma once
#include_next "ff.h"

#ifdef __cplusplus
#include <cstdint>
#include <type_traits>

namespace daisycola
{
// Integral types that are at most 32 bits on the STM32: everything but (unsigned) long long.
// size_t and long are 64 bits on x86-64 but 32 on the chip, so they wrap at 32 bits here.
template <class T>
inline constexpr bool kChip32Integral
    = std::is_integral_v<T>
      && !std::is_same_v<std::remove_cv_t<T>, long long>
      && !std::is_same_v<std::remove_cv_t<T>, unsigned long long>;

// A file size, as FatFs's 32-bit FSIZE_t.
struct ChipSize
{
    uint32_t v;
    operator uint32_t() const { return v; }
};

// `real * file size`, computed in F as on the chip. It converts only to uint32_t, with ARM's
// saturation. Any other use (comparisons, arithmetic other than + integral, conversion to another
// type) fails to compile instead of silently using x86 semantics.
template <class F>
struct ChipReal
{
    F f;

    operator uint32_t() const
    {
        if(!(f > F(0))) // negative, zero or NaN
            return 0;
        if(f >= F(4294967296.0))
            return 0xFFFFFFFFu;
        return static_cast<uint32_t>(f);
    }
    template <class T>
    operator T() const = delete;
};

template <class T, class = std::enable_if_t<kChip32Integral<T>>>
inline ChipSize operator-(ChipSize a, T b)
{
    return {uint32_t(a.v - uint32_t(b))};
}
template <class T, class = std::enable_if_t<kChip32Integral<T>>>
inline ChipSize operator+(ChipSize a, T b)
{
    return {uint32_t(a.v + uint32_t(b))};
}
template <class F, class = std::enable_if_t<std::is_floating_point_v<F>>>
inline ChipReal<F> operator*(F a, ChipSize b)
{
    return {a * F(b.v)};
}
template <class F, class = std::enable_if_t<std::is_floating_point_v<F>>>
inline ChipReal<F> operator*(ChipSize a, F b)
{
    return {F(a.v) * b};
}
template <class F, class T, class = std::enable_if_t<kChip32Integral<T>>>
inline ChipReal<F> operator+(ChipReal<F> a, T b)
{
    using Chip = std::conditional_t<std::is_signed_v<T>, int32_t, uint32_t>;
    return {a.f + F(Chip(b))};
}
} // namespace daisycola

#undef f_size
#define f_size(fp) (::daisycola::ChipSize{uint32_t((fp)->obj.objsize)})
#endif
