// types.h - BFL types, shared by the semantic pass and the code generator.
//
// Types are interned: two types are the same exactly when their pointers are
// equal.  Sizes are in bytes, which is also cells for values held in cells.
#pragma once
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace bfl {

struct Type;

struct Field {
    std::string name;
    const Type* type;
    int         offset;          // bytes from the start of the struct
};

struct StructInfo {
    std::string        name;
    std::vector<Field> fields;
    int                size = 0;
    bool               complete = false;
    const Field* field(const std::string& n) const {
        for (const Field& f : fields) if (f.name == n) return &f;
        return nullptr;
    }
};

enum class TK { Void, Int, Ptr, Array, Struct, Error };

struct Type {
    TK          kind;
    int         size = 0;        // bytes; arrays: element size * count
    bool        is_signed = false;
    const Type* elem = nullptr;  // Ptr: pointee; Array: element
    int         count = 0;       // Array: elements
    const StructInfo* st = nullptr;
    std::string name;            // for messages

    bool is_int() const    { return kind == TK::Int; }
    bool is_ptr() const    { return kind == TK::Ptr; }
    bool is_scalar() const { return kind == TK::Int || kind == TK::Ptr; }
    bool is_error() const  { return kind == TK::Error; }
};

class Types {
public:
    Types() {
        void_ = make(TK::Void, 0, false, "void");
        err_  = make(TK::Error, 1, false, "<error>");
        u8_  = make(TK::Int, 1, false, "byte");
        s8_  = make(TK::Int, 1, true,  "sbyte");
        u16_ = make(TK::Int, 2, false, "word");
        s16_ = make(TK::Int, 2, true,  "int");
        u32_ = make(TK::Int, 4, false, "dword");
        s32_ = make(TK::Int, 4, true,  "long");
    }
    const Type* void_t() const { return void_; }
    const Type* error() const  { return err_; }
    const Type* u8() const  { return u8_; }
    const Type* s8() const  { return s8_; }
    const Type* u16() const { return u16_; }
    const Type* s16() const { return s16_; }
    const Type* u32() const { return u32_; }
    const Type* s32() const { return s32_; }
    const Type* integer(int size, bool sign) const {
        if (size == 1) return sign ? s8_ : u8_;
        if (size == 2) return sign ? s16_ : u16_;
        return sign ? s32_ : u32_;
    }
    const Type* by_name(const std::string& n) const {
        if (n == "byte") return u8_;
        if (n == "sbyte") return s8_;
        if (n == "word") return u16_;
        if (n == "int") return s16_;
        if (n == "dword") return u32_;
        if (n == "long") return s32_;
        if (n == "string") return ptr(u8_);
        return nullptr;
    }
    const Type* ptr(const Type* to) const {
        auto it = ptrs_.find(to);
        if (it != ptrs_.end()) return it->second;
        Type* t = make(TK::Ptr, 2, false, to->name + "*");
        t->elem = to;
        ptrs_[to] = t;
        return t;
    }
    const Type* array(const Type* of, int n) const {
        auto key = std::make_pair(of, n);
        auto it = arrays_.find(key);
        if (it != arrays_.end()) return it->second;
        Type* t = make(TK::Array, of->size * n, false, of->name + "[" + std::to_string(n) + "]");
        t->elem = of; t->count = n;
        arrays_[key] = t;
        return t;
    }
    StructInfo* new_struct(const std::string& n) {
        structs_.push_back(std::make_unique<StructInfo>());
        structs_.back()->name = n;
        return structs_.back().get();
    }
    const Type* struct_type(const StructInfo* s) const {
        auto it = stypes_.find(s);
        if (it != stypes_.end()) return it->second;
        Type* t = make(TK::Struct, s->size, false, s->name);
        t->st = s;
        stypes_[s] = t;
        return t;
    }
    void finish_struct(StructInfo* s) {          // fix the size once fields are known
        s->complete = true;
        auto it = stypes_.find(s);
        if (it != stypes_.end()) const_cast<Type*>(it->second)->size = s->size;
    }

private:
    Type* make(TK k, int size, bool sign, const std::string& name) const {
        store_.push_back(Type{k, size, sign, nullptr, 0, nullptr, name});
        return &store_.back();
    }
    mutable std::deque<Type> store_;
    mutable std::map<const Type*, const Type*> ptrs_;
    mutable std::map<std::pair<const Type*, int>, const Type*> arrays_;
    mutable std::map<const StructInfo*, const Type*> stypes_;
    std::vector<std::unique_ptr<StructInfo>> structs_;
    const Type *void_, *err_, *u8_, *s8_, *u16_, *s16_, *u32_, *s32_;
};

// Range of an integer type.
inline int64_t type_min(const Type* t) { return t->is_signed ? -(int64_t(1) << (t->size * 8 - 1)) : 0; }
inline int64_t type_max(const Type* t) { return t->is_signed ? (int64_t(1) << (t->size * 8 - 1)) - 1 : (int64_t(1) << (t->size * 8)) - 1; }
inline bool fits(int64_t v, const Type* t) { return v >= type_min(t) && v <= type_max(t); }
// v reduced to the type's width and sign (two's complement wrap).
inline int64_t wrap(int64_t v, const Type* t) {
    int bits = t->size * 8;
    uint64_t m = bits >= 64 ? ~0ull : ((1ull << bits) - 1);
    uint64_t u = (uint64_t)v & m;
    if (t->is_signed && (u >> (bits - 1)) & 1) return (int64_t)(u | ~m);
    return (int64_t)u;
}
// mulshr(x, y, k) at type t (at most 4 bytes): floor(x * y / 2^k) of the exact product of x and y
// (both of type t), wrapped to t.
inline int64_t mulshr_at(int64_t x, int64_t y, int64_t k, const Type* t) {
    x = wrap(x, t); y = wrap(y, t);
    if (t->is_signed) { int64_t p = x * y; return wrap(k >= 63 ? (p < 0 ? -1 : 0) : p >> k, t); }
    uint64_t p = (uint64_t)x * (uint64_t)y;
    return wrap(k >= 64 ? 0 : (int64_t)(p >> k), t);
}

} // namespace bfl
