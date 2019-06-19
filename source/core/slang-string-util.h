#ifndef SLANG_CORE_STRING_UTIL_H
#define SLANG_CORE_STRING_UTIL_H

#include "slang-string.h"
#include "slang-list.h"

#include <stdarg.h>

#include "../../slang-com-helper.h"
#include "../../slang-com-ptr.h"

namespace Slang {

/** A blob that uses a `String` for its storage.
*/
class StringBlob : public ISlangBlob, public RefObject
{
public:
    // ISlangUnknown
    SLANG_REF_OBJECT_IUNKNOWN_ALL

        // ISlangBlob
    SLANG_NO_THROW void const* SLANG_MCALL getBufferPointer() SLANG_OVERRIDE { return m_string.getBuffer(); }
    SLANG_NO_THROW size_t SLANG_MCALL getBufferSize() SLANG_OVERRIDE { return m_string.getLength(); }

        /// Get the contained string
    SLANG_FORCE_INLINE const String& getString() const { return m_string; }

    explicit StringBlob(String const& string)
        : m_string(string)
    {}

protected:
    ISlangUnknown* getInterface(const Guid& guid);
    String m_string;
};

struct StringUtil
{
        /// Split in, by specified splitChar into slices out
        /// Slices contents will directly address into in, so contents will only stay valid as long as in does.
    static void split(const UnownedStringSlice& in, char splitChar, List<UnownedStringSlice>& slicesOut);

        /// Equivalent to doing a split and then finding the index of 'find' on the array
        /// Returns -1 if not found
    static int indexOfInSplit(const UnownedStringSlice& in, char splitChar, const UnownedStringSlice& find);

        /// Given the entry at the split index specified.
        /// Will return slice with begin() == nullptr if not found or input has begin() == nullptr)
    static UnownedStringSlice getAtInSplit(const UnownedStringSlice& in, char splitChar, int index);

        /// Returns the size in bytes needed to hold the formatted string using the specified args, NOT including a terminating 0
        /// NOTE! The caller *should* assume this will consume the va_list (use va_copy to make a copy to be consumed)
    static size_t calcFormattedSize(const char* format, va_list args);

        /// Calculate the formatted string using the specified args.
        /// NOTE! The caller *should* assume this will consume the va_list
        /// The buffer should be at least calcFormattedSize + 1 bytes. The +1 is needed because a terminating 0 is written. 
    static void calcFormatted(const char* format, va_list args, size_t numChars, char* dst);

        /// Appends formatted string with args into buf
    static void append(const char* format, va_list args, StringBuilder& buf);

        /// Appends the formatted string with specified trailing args
    static void appendFormat(StringBuilder& buf, const char* format, ...);

        /// Create a string from the format string applying args (like sprintf)
    static String makeStringWithFormat(const char* format, ...);

        /// Given a string held in a blob, returns as a String
        /// Returns an empty string if blob is nullptr 
    static String getString(ISlangBlob* blob);

        /// Given a string or slice, replaces all instances of fromChar with toChar
    static String calcCharReplaced(const UnownedStringSlice& slice, char fromChar, char toChar);
    static String calcCharReplaced(const String& string, char fromChar, char toChar);
    
        /// Create a blob from a string
    static ComPtr<ISlangBlob> createStringBlob(const String& string);

        /// Returns a line extracted from the start of ioText.
        /// 
        /// At the end of all the text a 'special' null UnownedStringSlice with a null 'begin' pointer is returned.
        /// The slice passed in will be modified on output to contain the remaining text, starting at the beginning of the next line.
        /// As en empty final line is still a line, the special null UnownedStringSlice is the last value ioText after the last valid line is returned.
        /// 
        /// NOTE! That behavior is as if line terminators (like \n) act as separators. Thus input of "\n" will return *two* lines - an empty line
        /// before and then after the \n. 
    static UnownedStringSlice extractLine(UnownedStringSlice& ioText);
    
        /// Given text, splits into lines stored in outLines. NOTE! That lines is only valid as long as textIn remains valid
    static void calcLines(const UnownedStringSlice& textIn, List<UnownedStringSlice>& lines);

        /// Equal if the lines are equal (in effect a way to ignore differences in line breaks)
    static bool areLinesEqual(const UnownedStringSlice& a, const UnownedStringSlice& b);
};

/* A helper class that allows parsing of lines from text with iteration. Uses StringUtil::extractLine for the actual underlying implementation. */
class LineParser
{
public:
    struct Iterator
    {
        const UnownedStringSlice& operator*() const { return m_line; }
        const UnownedStringSlice* operator->() const { return &m_line; }
        Iterator& operator++()
        {
            m_line = StringUtil::extractLine(m_remaining);
            return *this;
        }
        Iterator operator++(int) { Iterator rs = *this; operator++(); return rs; }

            /// Equal if both are at the same m_line address exactly. Handles termination case correctly where line.begin() == nullptr.
        bool operator==(const Iterator& rhs) const { return m_line.begin() == rhs.m_line.begin();  }
        bool operator !=(const Iterator& rhs) const { return !(*this == rhs); }

            /// Ctor
        Iterator(const UnownedStringSlice& line, const UnownedStringSlice& remaining) : m_line(line), m_remaining(remaining) {}

    protected:
        UnownedStringSlice m_line;
        UnownedStringSlice m_remaining;
    };

    Iterator begin() const { UnownedStringSlice remaining(m_text);  UnownedStringSlice line = StringUtil::extractLine(remaining); return Iterator(line, remaining);  }
    Iterator end() const { UnownedStringSlice term(nullptr, nullptr); return Iterator(term, term); }

        /// Ctor
    LineParser(const UnownedStringSlice& text) : m_text(text) {}

protected:
    UnownedStringSlice m_text;
};

} // namespace Slang

#endif // SLANG_STRING_UTIL_H
