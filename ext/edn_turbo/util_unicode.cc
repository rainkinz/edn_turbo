#include <string>

//
// needed to define this in its own file because icu and ruby have
// differing definitions for Uchar and the compiler complains
//
#include <unicode/utypes.h>
#include <unicode/ustring.h>
#include <unicode/ucnv.h>
#include <unicode/unistr.h>

#include "util_unicode.h"

namespace edn
{
    namespace util
    {
        namespace unicode
        {
            //
            // unescapes any values that need to be replaced, saves it to utf8
            //
            bool to_utf8(const char *s, uint32_t len, std::string& rslt)
            {
                icu::UnicodeString ustr(s, len);

                if (ustr.isBogus()) {
                    return false;
                }

                ustr.unescape().toUTF8String(rslt);
                return true;
            }
        }
    }
}
