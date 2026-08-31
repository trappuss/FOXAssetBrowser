// FoxCity.h — the two CityHash entry points the Fox Engine hashes need.
// Implementation is google/cityhash's city.cc, vendored verbatim (see FoxCity.cpp).
#pragma once
#include <cstddef>
#include <cstdint>

namespace foxcity {
uint64_t CityHash64(const char* s, size_t len);
uint64_t CityHash64WithSeed(const char* s, size_t len, uint64_t seed);
uint64_t CityHash64WithSeeds(const char* s, size_t len, uint64_t seed0, uint64_t seed1);
}
