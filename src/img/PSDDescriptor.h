#ifndef IMG_PSDDESCRIPTOR_H
#define IMG_PSDDESCRIPTOR_H

#include <cstdint>
#include <map>
#include <string>
#include <vector>
#include "XC.h"

namespace img {

// Minimal reader for Photoshop's binary descriptor structure (the payload of blocks such as
// "lfx2" layer effects and "shmd" metadata).
//
// Layout, verified against the psd-tools implementation and against this repository's test
// file (docs: see the LC question in the design notes):
// The layer-effects block ("lfx2") is a DescriptorBlock2: two header words (version, then
// data_version == 16) precede the descriptor, which is what the default aStartOffset = 8
// skips. A bare descriptor (metadata blocks) parses with aStartOffset = 0.
//
//   descriptor : name (unicode string) + classID (key) + uint32 itemCount + items
//   key        : uint32 length; length 0 means a 4-byte term key, otherwise `length` bytes
//   item       : key + 4-byte OSType + value
//   value      : Objc/GlbO = nested descriptor, VlLs = uint32 count + count values,
//                doub = double, UntF = 4-byte unit + double, TEXT = unicode string,
//                enum = two keys, long = int32, comp = int64, bool = 1 byte,
//                tdta/alis = uint32 length + bytes, type/GlbC/Clss = name + key
//   unicode string: uint32 character count + count * 2 bytes UTF-16BE (no padding)
// Anything else fails the parse instead of guessing (the caller treats that as "no
// descriptor").
class PSDDescriptor {
public:
    struct Value {
        std::string type;  // the 4CC OSType, e.g. "Objc", "doub", "UntF", "enum", "bool"
        double number = 0.0;          // doub / long / comp / bool (0 or 1) / UntF value
        std::string unit;             // UntF unit 4CC, or an enum's type key
        std::string text;             // TEXT payload, an enum's value key, or a class name
        std::vector<std::pair<std::string, Value>> items;  // Objc / GlbO / VlLs members

        bool isValid() const { return !type.empty(); }
        // members of a descriptor or list value
        const Value* find(const char* aKey) const;
        const Value* at(size_t aIndex) const;
        size_t size() const { return items.size(); }
        // accessors that treat a missing / mistyped value as absent
        const char* typeName() const { return type.c_str(); }
        bool isDescriptor() const { return type == "Objc" || type == "GlbO"; }
        bool isList() const { return type == "VlLs"; }
        bool isNumber() const { return type == "doub" || type == "long" || type == "comp" || type == "UntF"; }
        bool asBool() const { return number != 0.0; }
        double asNumber(double aDefault = 0.0) const { return isNumber() ? number : aDefault; }
        std::string asText() const { return type == "TEXT" ? text : std::string(); }
        std::string asEnumValue() const { return type == "enum" ? text : std::string(); }
    };

    PSDDescriptor();

    // false when the buffer is not a descriptor (or uses a type this reader does not know)
    bool parse(const uint8* aData, size_t aSize, size_t aStartOffset = 8);

    const Value& root() const { return mRoot; }
    bool isValid() const { return mValid; }

private:
    bool readDescriptor(Value& aOut);
    bool readValue(Value& aOut, int aDepth);
    bool readKey(std::string& aOut);
    bool readUnicodeString(std::string& aOut);
    bool readBytes(size_t aCount);
    bool skip(size_t aCount);

    const uint8* mData;
    size_t mSize;
    size_t mPos;
    Value mRoot;
    bool mValid;
};

} // namespace img

#endif // IMG_PSDDESCRIPTOR_H
