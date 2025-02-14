/*
   +----------------------------------------------------------------------+
   | HipHop for PHP                                                       |
   +----------------------------------------------------------------------+
   | Copyright (c) 2010-2015 Facebook, Inc. (http://www.facebook.com)     |
   +----------------------------------------------------------------------+
   | This source file is subject to version 3.01 of the PHP license,      |
   | that is bundled with this package in the file LICENSE, and is        |
   | available through the world-wide-web at the following url:           |
   | http://www.php.net/license/3_01.txt                                  |
   | If you did not receive a copy of the PHP license and are unable to   |
   | obtain it through the world-wide-web, please send a note to          |
   | license@php.net so we can mail you a copy immediately.               |
   +----------------------------------------------------------------------+
*/

#ifndef incl_HPHP_JIT_TYPE_INL_H_
#error "type-inl.h should only be included by type.h"
#endif

#include "hphp/runtime/base/string-data.h"
#include "hphp/runtime/vm/class.h"

#include "hphp/util/hash.h"

#include <cstring>

namespace HPHP { namespace jit {
///////////////////////////////////////////////////////////////////////////////

///////////////////////////////////////////////////////////////////////////////
// Predefined Types

constexpr inline Type::Type(bits_t bits, Ptr kind)
  : m_bits(bits)
  , m_ptrKind(static_cast<ptr_t>(kind))
  , m_hasConstVal(false)
  , m_extra(0)
{}

#define IRT(name, ...) constexpr Type T##name{Type::k##name, Ptr::NotPtr};
#define IRTP(name, ptr, bits) constexpr Type T##name{Type::bits, Ptr::ptr};
IRT_PHP(IRT_BOXES_AND_PTRS)
IRT_PHP_UNIONS(IRT_BOXES_AND_PTRS)
IRT_SPECIAL
#undef IRT
#undef IRTP

#define IRT(name, ...) constexpr Type T##name{Type::k##name, Ptr::Bottom};
IRT_RUNTIME
#undef IRT

/*
 * Abbreviated namespace for predefined types.
 *
 * Used for macro codegen for types declared in ir.specification.
 */
namespace TypeNames {
#define IRT(name, ...) UNUSED constexpr Type name = T##name;
#define IRTP(name, ...) IRT(name)
  IR_TYPES
#undef IRT
#undef IRTP
};

namespace type_detail {
///////////////////////////////////////////////////////////////////////////////

/*
 * Widen a constant value if needed.
 */
template<class T>
using needs_promotion = std::integral_constant<
  bool,
  (std::is_integral<T>::value ||
   std::is_same<T,bool>::value ||
   std::is_enum<T>::value)
>;

template<class T>
typename std::enable_if<needs_promotion<T>::value,uint64_t>::type
promote_cns_if_needed(T t) { return static_cast<uint64_t>(t); }

template<class T>
typename std::enable_if<!needs_promotion<T>::value,T>::type
promote_cns_if_needed(T t) { return t; }

/*
 * Return the Type to use for a given C++ value.
 *
 * The only interesting case is int/bool disambiguation.  Enums are treated as
 * ints.
 */
template<class T>
typename std::enable_if<
  std::is_integral<T>::value || std::is_enum<T>::value,
  Type
>::type for_const(T) {
  return std::is_same<T,bool>::value ? TBool : TInt;
}
inline Type for_const(const StringData* sd) {
  assertx(sd->isStatic());
  return TStaticStr;
}
inline Type for_const(const ArrayData* ad) {
  assertx(ad->isStatic());
  return Type::StaticArray(ad->kind());
}
inline Type for_const(double)        { return TDbl; }
inline Type for_const(const Func*)   { return TFunc; }
inline Type for_const(const Class*)  { return TCls; }
inline Type for_const(ConstCctx)     { return TCctx; }
inline Type for_const(TCA)           { return TTCA; }

///////////////////////////////////////////////////////////////////////////////
}

///////////////////////////////////////////////////////////////////////////////
// Type.

inline Type::Type()
  : m_bits(kBottom)
  , m_ptrKind(static_cast<ptr_t>(Ptr::Bottom))
  , m_hasConstVal(false)
  , m_extra(0)
{}

inline Type::Type(DataType outer, DataType inner)
  : m_bits(bitsFromDataType(outer, inner))
  , m_ptrKind(static_cast<ptr_t>(outer == KindOfClass ? Ptr::Bottom
                                                      : Ptr::NotPtr))
  , m_hasConstVal(false)
  , m_extra(0)
{}

inline size_t Type::hash() const {
  return hash_int64_pair(m_raw, m_extra);
}

///////////////////////////////////////////////////////////////////////////////
// Comparison.

inline bool Type::operator==(Type rhs) const {
  return m_raw == rhs.m_raw && m_extra == rhs.m_extra;
}

inline bool Type::operator!=(Type rhs) const {
  return !operator==(rhs);
}

inline bool Type::operator>=(Type rhs) const {
  return rhs <= *this;
}

inline bool Type::operator<(Type rhs) const {
  return *this != rhs && *this <= rhs;
}

inline bool Type::operator>(Type rhs) const {
  return rhs < *this;
}

template<typename... Types>
bool Type::subtypeOfAny(Type t2, Types... ts) const {
  return *this <= t2 || subtypeOfAny(ts...);
}
inline bool Type::subtypeOfAny() const {
  return false;
}

inline bool Type::maybe(Type t2) const {
  return (*this & t2) != TBottom;
}

///////////////////////////////////////////////////////////////////////////////
// Is-a methods.

inline bool Type::isUnion() const {
  // This will return true iff more than 1 bit is set in m_bits.
  return (m_bits & (m_bits - 1)) != 0;
}

inline bool Type::isKnownDataType() const {
  assertx(*this <= TStkElem);

  // Some unions correspond to single KindOfs.
  return subtypeOfAny(TStr, TArr, TBoxedCell) || !isUnion();
}

inline bool Type::needsReg() const {
  return *this <= TStkElem && !isKnownDataType();
}

inline bool Type::isSimpleType() const {
  return subtypeOfAny(TBool, TInt, TDbl, TNull);
}

inline bool Type::isReferenceType() const {
  return subtypeOfAny(TStr, TArr, TObj, TRes);
}

///////////////////////////////////////////////////////////////////////////////
// Constant type creation.

template<typename T>
Type Type::cns(T val, Type ret) {
  assertx(!ret.m_hasConstVal);
  ret.m_hasConstVal = true;

  static_assert(sizeof(T) <= sizeof(ret),
                "Constant data was larger than supported");
  static_assert(std::is_pod<T>::value,
                "Constant data wasn't a pod");

  const auto toCopy = type_detail::promote_cns_if_needed(val);
  static_assert(sizeof(toCopy) == 8,
                "Unexpected size for toCopy");

  std::memcpy(&ret.m_extra, &toCopy, sizeof(toCopy));
  return ret;
}

template<typename T>
Type Type::cns(T val) {
  return cns(val, type_detail::for_const(val));
}

inline Type Type::cns(std::nullptr_t) {
  return TNullptr;
}

inline Type Type::cns(const TypedValue& tv) {
  if (tv.m_type == KindOfUninit) return TUninit;
  if (tv.m_type == KindOfNull)   return TInitNull;

  auto ret = [&] {
    switch (tv.m_type) {
      case KindOfUninit:
      case KindOfNull:
        not_reached();

      case KindOfClass:
      case KindOfBoolean:
      case KindOfInt64:
      case KindOfDouble:
      case KindOfStaticString:
        return Type(tv.m_type);

      case KindOfString:
        return type_detail::for_const(tv.m_data.pstr);

      case KindOfArray:
        return type_detail::for_const(tv.m_data.parr);

      case KindOfObject:
      case KindOfResource:
      case KindOfRef:
        always_assert(false && "Invalid KindOf for constant TypedValue");
    }
    not_reached();
  }();
  ret.m_hasConstVal = true;
  ret.m_extra = tv.m_data.num;
  return ret;
}

inline Type Type::dropConstVal() const {
  if (!m_hasConstVal) return *this;
  assertx(!isUnion());

  if (*this <= TStaticArr) {
    return Type::StaticArray(arrVal()->kind());
  }
  return Type(m_bits, ptrKind());
}

///////////////////////////////////////////////////////////////////////////////
// Constant introspection.

inline bool Type::hasConstVal() const {
  return m_hasConstVal;
}

inline bool Type::hasConstVal(Type t) const {
  return hasConstVal() && *this <= t;
}

template<typename T>
bool Type::hasConstVal(T val) const {
  return hasConstVal(cns(val));
}

inline uint64_t Type::rawVal() const {
  assertx(hasConstVal());
  return m_extra;
}

#define IMPLEMENT_CNS_VAL(TypeName, name, valtype)      \
  inline valtype Type::name##Val() const {              \
    assertx(hasConstVal(TypeName));                      \
    return m_##name##Val;                               \
  }

IMPLEMENT_CNS_VAL(TBool,       bool, bool)
IMPLEMENT_CNS_VAL(TInt,        int,  int64_t)
IMPLEMENT_CNS_VAL(TDbl,        dbl,  double)
IMPLEMENT_CNS_VAL(TStaticStr,  str,  const StringData*)
IMPLEMENT_CNS_VAL(TStaticArr,  arr,  const ArrayData*)
IMPLEMENT_CNS_VAL(TFunc,       func, const HPHP::Func*)
IMPLEMENT_CNS_VAL(TCls,        cls,  const Class*)
IMPLEMENT_CNS_VAL(TCctx,       cctx, ConstCctx)
IMPLEMENT_CNS_VAL(TTCA,        tca,  jit::TCA)
IMPLEMENT_CNS_VAL(TRDSHandle,  rdsHandle,  rds::Handle)

#undef IMPLEMENT_CNS_VAL

///////////////////////////////////////////////////////////////////////////////
// Specialized type creation.

inline Type Type::Array(ArrayData::ArrayKind kind) {
  return Type(TArr, ArraySpec(kind));
}

inline Type Type::Array(const RepoAuthType::Array* rat) {
  return Type(TArr, ArraySpec(rat));
}

inline Type Type::Array(const Shape* shape) {
  return Type(TArr, ArraySpec(shape));
}

inline Type Type::StaticArray(ArrayData::ArrayKind kind) {
  return Type(TStaticArr, ArraySpec(kind));
}

inline Type Type::StaticArray(const RepoAuthType::Array* rat) {
  return Type(TStaticArr, ArraySpec(rat));
}

inline Type Type::StaticArray(const Shape* shape) {
  return Type(TStaticArr, ArraySpec(shape));
}

inline Type Type::SubObj(const Class* cls) {
  if (cls->attrs() & AttrNoOverride) return ExactObj(cls);
  return Type(TObj, ClassSpec(cls, ClassSpec::SubTag{}));
}

inline Type Type::ExactObj(const Class* cls) {
  return Type(TObj, ClassSpec(cls, ClassSpec::ExactTag{}));
}

inline Type Type::SubCls(const Class* cls) {
  if (cls->attrs() & AttrNoOverride) return ExactCls(cls);
  return Type(TCls, ClassSpec(cls, ClassSpec::SubTag{}));
}

inline Type Type::ExactCls(const Class* cls) {
  return Type::cns(cls);
}

inline Type Type::unspecialize() const {
  return Type(m_bits, ptrKind());
}

///////////////////////////////////////////////////////////////////////////////
// Specialization introspection.

inline bool Type::isSpecialized() const {
  return clsSpec() || arrSpec();
}

inline bool Type::supports(bits_t bits, SpecKind kind) {
  switch (kind) {
    case SpecKind::None:
      return true;
    case SpecKind::Array:
      return bits & kArrSpecBits;
    case SpecKind::Class:
      return bits & kClsSpecBits;
  }
  not_reached();
}

inline bool Type::supports(SpecKind kind) const {
  return supports(m_bits, kind);
}

inline ArraySpec Type::arrSpec() const {
  if (!supports(SpecKind::Array)) return ArraySpec::Bottom;

  // Currently, a Type which supports multiple specializations is trivial along
  // all of them.
  if (supports(SpecKind::Class)) return ArraySpec::Top;

  if (m_hasConstVal) {
    return ArraySpec(arrVal()->kind());
  }
  assertx(m_arrSpec != ArraySpec::Bottom);
  return m_arrSpec;
}

inline ClassSpec Type::clsSpec() const {
  if (!supports(SpecKind::Class)) return ClassSpec::Bottom;

  // Currently, a Type which supports multiple specializations is trivial along
  // all of them.
  if (supports(SpecKind::Array)) return ClassSpec::Top;

  if (m_hasConstVal) {
    return ClassSpec(clsVal(), ClassSpec::ExactTag{});
  }
  assertx(m_clsSpec != ClassSpec::Bottom);
  return m_clsSpec;
}

inline TypeSpec Type::spec() const {
  return TypeSpec(arrSpec(), clsSpec());
}

///////////////////////////////////////////////////////////////////////////////
// Inner types.

inline Type Type::box() const {
  assertx(*this <= TCell);
  // Boxing Uninit returns InitNull but that logic doesn't belong here.
  assertx(!maybe(TUninit) || *this == TCell);
  return Type(m_bits << kBoxShift,
              ptrKind(),
              isSpecialized() && !m_hasConstVal ? m_extra : 0);
}

inline Type Type::inner() const {
  assertx(*this <= TBoxedCell);
  return Type(m_bits >> kBoxShift, ptrKind(), m_extra);
}

inline Type Type::unbox() const {
  assertx(*this <= TGen);
  return (*this & TCell) | (*this & TBoxedCell).inner();
}

inline Type Type::ptr(Ptr kind) const {
  assertx(*this <= TGen);
  assertx(kind <= Ptr::Ptr);
  // Enforce a canonical representation for Bottom.
  if (m_bits == kBottom) return TBottom;
  return Type(m_bits,
              kind,
              isSpecialized() && !m_hasConstVal ? m_extra : 0);
}

inline Type Type::deref() const {
  assertx(*this <= TPtrToGen);
  if (m_bits == kBottom) return TBottom;
  return Type(m_bits,
              Ptr::NotPtr,
              isSpecialized() ? m_extra : 0);
}

inline Type Type::derefIfPtr() const {
  assertx(*this <= (TGen | TPtrToGen));
  return *this <= TPtrToGen ? deref() : *this;
}

inline Type Type::strip() const {
  return derefIfPtr().unbox();
}

inline Ptr Type::ptrKind() const {
  return static_cast<Ptr>(m_ptrKind);
}

///////////////////////////////////////////////////////////////////////////////
// Private constructors.

inline Type::Type(bits_t bits, Ptr kind, uintptr_t extra)
  : m_bits(bits)
  , m_ptrKind(static_cast<ptr_t>(kind))
  , m_hasConstVal(false)
  , m_extra(extra)
{
  assertx(checkValid());
}

inline Type::Type(Type t, ArraySpec arraySpec)
  : m_bits(t.m_bits)
  , m_ptrKind(t.m_ptrKind)
  , m_hasConstVal(false)
  , m_arrSpec(arraySpec)
{
  assertx(checkValid());
  assertx(m_arrSpec != ArraySpec::Bottom);
}

inline Type::Type(Type t, ClassSpec classSpec)
  : m_bits(t.m_bits)
  , m_ptrKind(t.m_ptrKind)
  , m_hasConstVal(false)
  , m_clsSpec(classSpec)
{
  assertx(checkValid());
  assertx(m_clsSpec != ClassSpec::Bottom);
}

///////////////////////////////////////////////////////////////////////////////

}}
