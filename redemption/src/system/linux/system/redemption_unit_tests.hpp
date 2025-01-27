#pragma once

#include <boost/test/unit_test.hpp>

#include "cxx/diagnostic.hpp"
#include "cxx/compiler_version.hpp"

// fixed link error (API changed)
// #ifdef __clang__
// namespace boost { namespace unit_test { namespace ut_detail {
//     std::string normalize_test_case_name(const_string name) {
//         return ( name[0] == '&' ? std::string(name.begin()+1, name.size()-1) : std::string(name.begin(), name.size() ));
//     }
// }}}
// #endif

// FIXME global warning deactivation, should be in each macro
#if REDEMPTION_COMP_CLANG_VERSION >= REDEMPTION_COMP_VERSION_NUMBER(9, 0, 0)
    REDEMPTION_DIAGNOSTIC_PUSH()
    REDEMPTION_DIAGNOSTIC_CLANG_IGNORE("-Wused-but-marked-unused")
#endif

#ifdef IN_IDE_PARSER
# define FIXTURES_PATH "./tests/fixtures"
# define CFG_PATH "./sys/etc/rdpproxy"
# define CHECK_EXCEPTION_ERROR_ID(stmt, id) do { stmt; id; } while (0)
# define BOOST_CHECK_NO_THROW(stmt) do { stmt; } while (0)
# define BOOST_CHECK_THROW(stmt, exception) do { stmt; [](exception) {}; } while (0)
# define BOOST_CHECK_EQUAL(a, b) (a) == (b)
# define BOOST_CHECK_LT(a, b) (a) < (b)
# define BOOST_CHECK_NE(a, b) (a) != (b)
# define BOOST_CHECK(a, b) (a) != (b)
# define BOOST_CHECK_EQUAL_RANGES(a, b) (a) != (b)
# define BOOST_REQUIRE_NO_THROW(stmt) do { stmt; } while (0)
# define BOOST_REQUIRE_THROW(stmt, exception) do { stmt; [](exception) {}; } while (0)
# define BOOST_REQUIRE_EQUAL(a, b) (a) == (b)
# define BOOST_REQUIRE_NE(a, b) (a) != (b)
# define BOOST_REQUIRE_EQUAL_RANGES(a, b) (a) != (b)
#else
# define CHECK_EXCEPTION_ERROR_ID(stmt, ErrId)  \
    BOOST_CHECK_EXCEPTION(                      \
        stmt, Error,                            \
        [&](Error const & e) {                  \
            if (e.id == ErrId) {                \
                BOOST_CHECK_EQUAL(e.id, ErrId); \
                return true;                    \
            }                                   \
            LOG(LOG_ERR, "Exception=%d", e.id); \
            return false;                       \
        }                                       \
    )
# define BOOST_CHECK_EQUAL_RANGES(a_, b_)               \
    do {                                                \
        auto const & A_CHECK_RANGES = a_;               \
        auto const & B_CHECK_RANGES = b_;               \
        using std::begin;                               \
        using std::end;                                 \
        BOOST_CHECK_EQUAL_COLLECTIONS(                  \
            begin(A_CHECK_RANGES), end(A_CHECK_RANGES), \
            begin(B_CHECK_RANGES), end(B_CHECK_RANGES)  \
        );                                              \
    } while (0)
# define BOOST_REQUIRE_EQUAL_RANGES(a_, b_)             \
    do {                                                \
        auto const & A_CHECK_RANGES = a_;               \
        auto const & B_CHECK_RANGES = b_;               \
        using std::begin;                               \
        using std::end;                                 \
        BOOST_REQUIRE_EQUAL_COLLECTIONS(                \
            begin(A_CHECK_RANGES), end(A_CHECK_RANGES), \
            begin(B_CHECK_RANGES), end(B_CHECK_RANGES)  \
        );                                              \
    } while (0)
#endif
