#pragma once

#include <cstddef>
#include <cstdint>

namespace ontodb {

// Fixed for the whole system. Every on-disk page is this long, and the first
// kPageLsnSize bytes of every page are the pageLSN (little-endian). Payload
// layouts in later phases start at kPagePayloadOffset.
inline constexpr size_t PAGE_SIZE = 4096;
inline constexpr size_t kPageLsnOffset = 0;
inline constexpr size_t kPageLsnSize = 8;
inline constexpr size_t kPagePayloadOffset = 8;

inline constexpr size_t kDefaultPoolSize = 64;
inline constexpr size_t kDefaultLruK = 2;

inline constexpr char kRdfType[] = "http://www.w3.org/1999/02/22-rdf-syntax-ns#type";
inline constexpr char kXsdInteger[] = "http://www.w3.org/2001/XMLSchema#integer";
inline constexpr char kXsdDecimal[] = "http://www.w3.org/2001/XMLSchema#decimal";
inline constexpr char kXsdDouble[] = "http://www.w3.org/2001/XMLSchema#double";
inline constexpr char kXsdFloat[] = "http://www.w3.org/2001/XMLSchema#float";

}  // namespace ontodb
