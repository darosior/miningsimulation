/** A fast, non cryptographically secure, RNG courtesy of Pieter Wuille who pointed
 * out that it would be much more efficient to use it instead of the standard library's.
 */
class RNG
{
    uint64_t m_s0;
    uint64_t m_s1;

    [[nodiscard]] constexpr static uint64_t SplitMix64(uint64_t& seedval) noexcept
    {
        uint64_t z = (seedval += 0x9e3779b97f4a7c15);
        z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9;
        z = (z ^ (z >> 27)) * 0x94d049bb133111eb;
        return z ^ (z >> 31);
    }

    static double MakeExponentiallyDistributed(uint64_t uniform) noexcept
    {
        return -std::log1p((uniform >> 11) * -0x1.0p-53);
    }

public:
    constexpr explicit RNG(uint64_t seedval) noexcept
        : m_s0(SplitMix64(seedval)), m_s1(SplitMix64(seedval)) {}

    constexpr uint64_t rand64() noexcept
    {
        uint64_t s0 = m_s0, s1 = m_s1;
        const uint64_t result = std::rotl(s0 + s1, 17) + s0;
        s1 ^= s0;
        m_s0 = std::rotl(s0, 49) ^ s1 ^ (s1 << 21);
        m_s1 = std::rotl(s1, 28);
        return result;
    }

    double exporand(double mean) noexcept
    {
        return mean * MakeExponentiallyDistributed(rand64());
    }
};
