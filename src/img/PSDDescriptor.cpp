#include <cstring>
#include "img/PSDDescriptor.h"

namespace img {

namespace {

constexpr int kMaxDepth = 16; // descriptors nest through effect parameters; 16 is generous

uint32_t readU32(const uint8* aData, size_t aSize, size_t aPos, bool& aOk) {
    if (aPos + 4 > aSize) {
        aOk = false;
        return 0;
    }
    aOk = true;
    return ((uint32_t)aData[aPos] << 24) | ((uint32_t)aData[aPos + 1] << 16) | ((uint32_t)aData[aPos + 2] << 8)
        | (uint32_t)aData[aPos + 3];
}

double readF64(const uint8* aData, size_t aSize, size_t aPos, bool& aOk) {
    if (aPos + 8 > aSize) {
        aOk = false;
        return 0.0;
    }
    uint64_t bits = 0;
    for (int i = 0; i < 8; ++i)
        bits = (bits << 8) | aData[aPos + i];
    double value = 0.0;
    std::memcpy(&value, &bits, sizeof(value));
    aOk = true;
    return value;
}

std::string toKey(const uint8* aData, size_t aPos, size_t aLen) {
    std::string key(reinterpret_cast<const char*>(aData + aPos), aLen);
    // trim trailing NULs so 4CC terms compare cleanly against literals
    while (!key.empty() && key.back() == '\0')
        key.pop_back();
    return key;
}

} // namespace

const PSDDescriptor::Value* PSDDescriptor::Value::find(const char* aKey) const {
    for (const auto& item : items) {
        if (item.first == aKey) {
            return &item.second;
        }
    }
    return nullptr;
}

const PSDDescriptor::Value* PSDDescriptor::Value::at(size_t aIndex) const {
    return aIndex < items.size() ? &items[aIndex].second : nullptr;
}

PSDDescriptor::PSDDescriptor(): mData(nullptr), mSize(0), mPos(0), mRoot(), mValid(false) {}

bool PSDDescriptor::readBytes(size_t aCount) {
    if (mPos + aCount > mSize) {
        return false;
    }
    mPos += aCount;
    return true;
}

bool PSDDescriptor::skip(size_t aCount) { return readBytes(aCount); }

bool PSDDescriptor::readUnicodeString(std::string& aOut) {
    bool ok = false;
    const uint32_t count = readU32(mData, mSize, mPos, ok);
    if (!ok || count > (mSize - mPos) / 2) {
        return false;
    }
    mPos += 4;
    aOut.clear();
    aOut.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        const uint16_t unit = (uint16_t)((mData[mPos] << 8) | mData[mPos + 1]);
        mPos += 2;
        // keep it simple: ASCII is what keys/names in effects use; anything else becomes '?'
        aOut.push_back(unit < 0x80 ? (char)unit : '?');
    }
    return true;
}

bool PSDDescriptor::readKey(std::string& aOut) {
    bool ok = false;
    const uint32_t length = readU32(mData, mSize, mPos, ok);
    if (!ok) {
        return false;
    }
    mPos += 4;
    const size_t len = length == 0 ? 4 : length;
    if (mPos + len > mSize) {
        return false;
    }
    aOut = toKey(mData, mPos, len);
    mPos += len;
    return true;
}

bool PSDDescriptor::readDescriptor(Value& aOut) {
    std::string name;
    std::string classID;
    if (!readUnicodeString(name) || !readKey(classID)) {
        return false;
    }

    bool ok = false;
    const uint32_t count = readU32(mData, mSize, mPos, ok);
    if (!ok) {
        return false;
    }
    mPos += 4;
    if (count > mSize) {
        return false; // a count larger than the remaining bytes cannot be right
    }

    aOut.type = "Objc";
    aOut.text = classID;
    aOut.items.clear();
    aOut.items.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        std::string key;
        if (!readKey(key)) {
            return false;
        }
        Value value;
        if (!readValue(value, 1)) {
            return false;
        }
        aOut.items.emplace_back(std::move(key), std::move(value));
    }
    return true;
}

bool PSDDescriptor::readValue(Value& aOut, int aDepth) {
    if (aDepth > kMaxDepth || mPos + 4 > mSize) {
        return false;
    }
    const std::string ostype = toKey(mData, mPos, 4);
    mPos += 4;
    aOut = Value();
    aOut.type = ostype;

    bool ok = false;
    if (ostype == "Objc" || ostype == "GlbO") {
        return readDescriptor(aOut);
    }
    if (ostype == "VlLs") {
        const uint32_t count = readU32(mData, mSize, mPos, ok);
        if (!ok || count > mSize) {
            return false;
        }
        mPos += 4;
        aOut.items.reserve(count);
        for (uint32_t i = 0; i < count; ++i) {
            Value item;
            if (!readValue(item, aDepth + 1)) {
                return false;
            }
            aOut.items.emplace_back(std::string(), std::move(item));
        }
        return true;
    }
    if (ostype == "doub") {
        aOut.number = readF64(mData, mSize, mPos, ok);
        if (!ok)
            return false;
        mPos += 8;
        return true;
    }
    if (ostype == "UntF") {
        if (mPos + 4 > mSize)
            return false;
        aOut.unit = toKey(mData, mPos, 4);
        mPos += 4;
        aOut.number = readF64(mData, mSize, mPos, ok);
        if (!ok)
            return false;
        mPos += 8;
        return true;
    }
    if (ostype == "TEXT") {
        return readUnicodeString(aOut.text);
    }
    if (ostype == "enum") {
        if (!readKey(aOut.unit) || !readKey(aOut.text)) {
            return false;
        }
        return true;
    }
    if (ostype == "long") {
        aOut.number = (double)(int32_t)readU32(mData, mSize, mPos, ok);
        if (!ok)
            return false;
        mPos += 4;
        return true;
    }
    if (ostype == "comp") {
        const uint64_t hi = readU32(mData, mSize, mPos, ok);
        if (!ok)
            return false;
        const uint64_t lo = readU32(mData, mSize, mPos + 4, ok);
        if (!ok)
            return false;
        aOut.number = (double)(int64_t)((hi << 32) | lo);
        mPos += 8;
        return true;
    }
    if (ostype == "bool") {
        if (mPos + 1 > mSize)
            return false;
        aOut.number = mData[mPos] != 0 ? 1.0 : 0.0;
        mPos += 1;
        return true;
    }
    if (ostype == "tdta" || ostype == "alis") {
        const uint32_t length = readU32(mData, mSize, mPos, ok);
        if (!ok)
            return false;
        mPos += 4;
        if (!readBytes(length))
            return false;
        aOut.text = "<raw>";
        return true;
    }
    if (ostype == "type" || ostype == "GlbC" || ostype == "Clss") {
        std::string name;
        std::string classID;
        if (!readUnicodeString(name) || !readKey(classID)) {
            return false;
        }
        aOut.text = name;
        aOut.unit = classID;
        return true;
    }
    if (ostype == "ObAr") {
        const uint32_t count = readU32(mData, mSize, mPos, ok);
        if (!ok)
            return false;
        mPos += 4;
        // an object array is a descriptor body followed by that many objects; only the body
        // matters for the effect blocks this reader serves
        Value body;
        if (!readDescriptor(body)) {
            return false;
        }
        aOut.items.reserve(count);
        for (uint32_t i = 0; i < count; ++i) {
            Value object;
            if (!readDescriptor(object)) {
                return false;
            }
            aOut.items.emplace_back(std::string(), std::move(object));
        }
        return true;
    }
    return false; // unknown OSType: fail rather than mis-parse the rest of the block
}

bool PSDDescriptor::parse(const uint8* aData, size_t aSize, size_t aStartOffset) {
    mData = aData;
    mSize = aSize;
    mPos = aStartOffset;
    mValid = false;
    mRoot = Value();
    if (!mData || aStartOffset >= mSize) {
        return false;
    }
    if (!readDescriptor(mRoot)) {
        return false;
    }
    mValid = true;
    return true;
}

} // namespace img
