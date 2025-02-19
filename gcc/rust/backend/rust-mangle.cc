#include "rust-mangle.h"
#include "fnv-hash.h"
#include "optional.h"
#include "rust-base62.h"
#include "rust-unicode.h"
#include "rust-diagnostics.h"
#include "rust-unicode.h"
#include "rust-punycode.h"

// FIXME: Rename those to legacy_*
static const std::string kMangledSymbolPrefix = "_ZN";
static const std::string kMangledSymbolDelim = "E";
static const std::string kMangledGenericDelim = "$C$";
static const std::string kMangledSubstBegin = "$LT$";
static const std::string kMangledSubstEnd = "$GT$";
static const std::string kMangledSpace = "$u20$";
static const std::string kMangledRef = "$RF$";
static const std::string kMangledPtr = "$BP$";
static const std::string kMangledLeftSqParen = "$u5b$";	 // [
static const std::string kMangledRightSqParen = "$u5d$"; // ]
static const std::string kMangledLeftBrace = "$u7b$";	 // {
static const std::string kMangledRightBrace = "$u7d$";	 // }
static const std::string kQualPathBegin = "_" + kMangledSubstBegin;
static const std::string kMangledComma = "$C$";

namespace Rust {
namespace Compile {

Mangler::MangleVersion Mangler::version = MangleVersion::LEGACY;

static std::string
legacy_mangle_name (const std::string &name)
{
  // example
  //  <&T as core::fmt::Debug>::fmt:
  //  _ZN42_$LT$$RF$T$u20$as$u20$core..fmt..Debug$GT$3fmt17h6dac924c0051eef7E
  // replace all white space with $ and & with RF
  //
  // <example::Bar as example::A>::fooA:
  // _ZN43_$LT$example..Bar$u20$as$u20$example..A$GT$4fooA17hfc615fa76c7db7a0E:
  //
  // core::ptr::const_ptr::<impl *const T>::cast:
  // _ZN4core3ptr9const_ptr33_$LT$impl$u20$$BP$const$u20$T$GT$4cast17hb79f4617226f1d55E:
  //
  // core::ptr::const_ptr::<impl *const [T]>::as_ptr:
  // _ZN4core3ptr9const_ptr43_$LT$impl$u20$$BP$const$u20$$u5b$T$u5d$$GT$6as_ptr17he16e0dcd9473b04fE:
  //
  // example::Foo<T>::new:
  // _ZN7example12Foo$LT$T$GT$3new17h9a2aacb7fd783515E:
  //
  // <example::Identity as example::FnLike<&T,&T>>::call
  // _ZN74_$LT$example..Identity$u20$as$u20$example..FnLike$LT$$RF$T$C$$RF$T$GT$$GT$4call17ha9ee58935895acb3E

  tl::optional<Utf8String> utf8_name = Utf8String::make_utf8_string (name);
  if (!utf8_name.has_value ())
    rust_unreachable ();
  std::vector<Codepoint> chars = utf8_name.value ().get_chars ();
  std::string buffer;
  for (size_t i = 0; i < chars.size (); i++)
    {
      std::string m;
      Codepoint c = chars.at (i);

      if (c == ' ')
	m = kMangledSpace;
      else if (c == '&')
	m = kMangledRef;
      else if (i == 0 && c == '<')
	m = kQualPathBegin;
      else if (c == '<')
	m = kMangledSubstBegin;
      else if (c == '>')
	m = kMangledSubstEnd;
      else if (c == '*')
	m = kMangledPtr;
      else if (c == '[')
	m = kMangledLeftSqParen;
      else if (c == ']')
	m = kMangledRightSqParen;
      else if (c == '{')
	m = kMangledLeftBrace;
      else if (c == '}')
	m = kMangledRightBrace;
      else if (c == ',')
	m = kMangledComma;
      else if (c == ':')
	{
	  rust_assert (i + 1 < chars.size ());
	  rust_assert (chars.at (i + 1) == ':');
	  i++;
	  m = "..";
	}
      else if (c.is_ascii ())
	// ASCII
	m.push_back (c.value);
      else
	{
	  // Non-ASCII
	  std::stringstream escaped;
	  escaped << std::hex << "$u" << c.value << "$";
	  m += escaped.str ();
	}
      buffer += m;
    }

  return std::to_string (buffer.size ()) + buffer;
}

static std::string
legacy_mangle_canonical_path (const Resolver::CanonicalPath &path)
{
  std::string buffer;
  for (size_t i = 0; i < path.size (); i++)
    {
      auto &seg = path.get_seg_at (i);
      buffer += legacy_mangle_name (seg.second);
    }
  return buffer;
}

// rustc uses a sip128 hash for legacy mangling, but an fnv 128 was quicker to
// implement for now
static std::string
legacy_hash (const std::string &fingerprint)
{
  Hash::FNV128 hasher;
  hasher.write ((const unsigned char *) fingerprint.c_str (),
		fingerprint.size ());

  uint64_t hi, lo;
  hasher.sum (&hi, &lo);

  char hex[16 + 1];
  memset (hex, 0, sizeof hex);
  snprintf (hex, sizeof hex, "%08" PRIx64 "%08" PRIx64, lo, hi);

  return "h" + std::string (hex, sizeof (hex) - 1);
}

static std::string
v0_tuple_prefix (const TyTy::BaseType *ty)
{
  if (ty->is_unit ())
    return "u";

  // FIXME: ARTHUR: Add rest of algorithm
  return "";
}

static std::string
v0_numeric_prefix (const TyTy::BaseType *ty)
{
  static const std::map<std::string, std::string> num_prefixes = {
    {"[i8]", "a"},    {"[u8]", "h"},	{"[i16]", "s"}, {"[u16]", "t"},
    {"[i32]", "l"},   {"[u32]", "m"},	{"[i64]", "x"}, {"[u64]", "y"},
    {"[isize]", "i"}, {"[usize]", "j"}, {"[f32]", "f"}, {"[f64]", "d"},
  };

  auto ty_kind = ty->get_kind ();
  auto ty_str = ty->as_string ();
  auto numeric_iter = num_prefixes.end ();

  // Special numeric types
  if (ty_kind == TyTy::TypeKind::ISIZE)
    return "i";
  else if (ty_kind == TyTy::TypeKind::USIZE)
    return "j";

  numeric_iter = num_prefixes.find (ty_str);
  if (numeric_iter != num_prefixes.end ())
    return numeric_iter->second;

  return "";
}

static std::string
v0_simple_type_prefix (const TyTy::BaseType *ty)
{
  switch (ty->get_kind ())
    {
    case TyTy::TypeKind::BOOL:
      return "b";
    case TyTy::TypeKind::CHAR:
      return "c";
    case TyTy::TypeKind::STR:
      return "e";
    case TyTy::TypeKind::NEVER:
      return "z";

      // Placeholder types
    case TyTy::TypeKind::ERROR:	      // Fallthrough
    case TyTy::TypeKind::INFER:	      // Fallthrough
    case TyTy::TypeKind::PLACEHOLDER: // Fallthrough
    case TyTy::TypeKind::PARAM:
      // FIXME: TyTy::TypeKind::BOUND is also a valid variant in rustc
      return "p";

    case TyTy::TypeKind::TUPLE:
      return v0_tuple_prefix (ty);

    case TyTy::TypeKind::UINT:	// Fallthrough
    case TyTy::TypeKind::INT:	// Fallthrough
    case TyTy::TypeKind::FLOAT: // Fallthrough
    case TyTy::TypeKind::ISIZE: // Fallthrough
    case TyTy::TypeKind::USIZE: // Fallthrough
      return v0_numeric_prefix (ty);

    default:
      return "";
    }

  rust_unreachable ();
}

// Add an underscore-terminated base62 integer to the mangling string.
// This corresponds to the `<base-62-number>` grammar in the v0 mangling RFC:
//  - 0 is encoded as "_"
//  - any other value is encoded as itself minus one in base 62, followed by
//  "_"
static void
v0_add_integer_62 (std::string &mangled, uint64_t x)
{
  if (x > 0)
    mangled.append (base62_integer (x - 1));

  mangled.append ("_");
}

// Add a tag-prefixed base62 integer to the mangling string when the
// integer is greater than 0:
//  - 0 is encoded as "" (nothing)
//  - any other value is encoded as <tag> + v0_add_integer_62(itself), that is
//  <tag> + base62(itself - 1) + '_'
static void
v0_add_opt_integer_62 (std::string &mangled, std::string tag, uint64_t x)
{
  if (x > 0)
    {
      mangled.append (tag);
      v0_add_integer_62 (mangled, x);
    }
}

static void
v0_add_disambiguator (std::string &mangled, uint64_t dis)
{
  v0_add_opt_integer_62 (mangled, "s", dis);
}

// Add an identifier to the mangled string. This corresponds to the
// `<identifier>` grammar in the v0 mangling RFC.
static void
v0_add_identifier (std::string &mangled, const std::string &identifier)
{
  // The grammar for unicode identifier is contained in
  // <undisambiguated-identifier>, right under the <identifier> one. If the
  // identifier contains unicode values, then an extra "u" needs to be added to
  // the mangling string and `punycode` must be used to encode the characters.
  tl::optional<Utf8String> uident_opt
    = Utf8String::make_utf8_string (identifier);
  rust_assert (uident_opt.has_value ());
  tl::optional<std::string> punycode_opt
    = encode_punycode (uident_opt.value ());
  rust_assert (punycode_opt.has_value ());

  bool is_ascii_ident = true;
  for (auto c : uident_opt.value ().get_chars ())
    if (c.value > 127)
      {
	is_ascii_ident = false;
	break;
      }

  std::string punycode = punycode_opt.value ();
  // remove tailing hyphen
  if (punycode.back () == '-')
    punycode.pop_back ();
  // replace hyphens in punycode with underscores
  std::replace (punycode.begin (), punycode.end (), '-', '_');

  if (!is_ascii_ident)
    mangled.append ("u");

  mangled += std::to_string (punycode.size ());
  // If the first character of the identifier is a digit or an underscore, we
  // add an extra underscore
  if (punycode[0] == '_')
    mangled += "_";

  mangled += punycode;
}

static std::string
v0_type_prefix (const TyTy::BaseType *ty)
{
  auto ty_prefix = v0_simple_type_prefix (ty);
  if (!ty_prefix.empty ())
    return ty_prefix;

  // FIXME: We need to fetch more type prefixes
  rust_unreachable ();
}

static std::string
legacy_mangle_item (const TyTy::BaseType *ty,
		    const Resolver::CanonicalPath &path)
{
  const std::string hash = legacy_hash (ty->mangle_string ());
  const std::string hash_sig = legacy_mangle_name (hash);

  return kMangledSymbolPrefix + legacy_mangle_canonical_path (path) + hash_sig
	 + kMangledSymbolDelim;
}

static std::string
v0_mangle_item (const TyTy::BaseType *ty, const Resolver::CanonicalPath &path)
{
  // we can get this from the canonical_path
  auto mappings = Analysis::Mappings::get ();
  std::string crate_name;
  bool ok = mappings->get_crate_name (path.get_crate_num (), crate_name);
  rust_assert (ok);

  std::string mangled;
  // FIXME: Add real algorithm once all pieces are implemented
  v0_add_identifier (mangled, crate_name);
  v0_add_disambiguator (mangled, 62);
  auto ty_prefix = v0_type_prefix (ty);

  rust_unreachable ();
}

std::string
Mangler::mangle_item (const TyTy::BaseType *ty,
		      const Resolver::CanonicalPath &path) const
{
  switch (version)
    {
    case Mangler::MangleVersion::LEGACY:
      return legacy_mangle_item (ty, path);
    case Mangler::MangleVersion::V0:
      return v0_mangle_item (ty, path);
    default:
      rust_unreachable ();
    }
}

} // namespace Compile
} // namespace Rust
