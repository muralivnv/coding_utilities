#ifndef KOI_SHA1_H_
#define KOI_SHA1_H_

#include <array>
#include <cstdint>
#include <string>
#include <string_view>

namespace koi {

// SHA-1, RFC 3174. Here for one reason: git names a blob by the SHA-1 of a
// header and its bytes, and a stored location wants that name so healing can
// ask "is the file still the one this row was recorded against?" in O(1). No
// dependency and no subprocess -- `git hash-object` per record would be a fork
// on the input loop.
//
// Not used for anything that has to resist an attacker, and there is nothing
// here that could be: the answer is compared against git's own naming of the
// same bytes, so the algorithm is fixed by what git does, not chosen.
std::array<std::uint8_t, 20> Sha1(std::string_view bytes);

// git's blob object id: sha1("blob " + decimal length + "\0" + bytes), as the
// forty lowercase hex characters every git command prints.
std::string GitBlobOid(std::string_view bytes);

}

#endif
