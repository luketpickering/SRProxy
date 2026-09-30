#include "declaration_templates.h"

#include "TBaseClass.h"
#include "TClass.h"
#include "TDataMember.h"
#include "TInterpreter.h"
#include "TMethod.h"
#include "TMethodArg.h"
#include "TSystem.h"

#include "spdlog/fmt/bundled/chrono.h"
#include "spdlog/fmt/bundled/core.h"
#include "spdlog/fmt/bundled/format.h"
#include "spdlog/spdlog.h"

#include <algorithm>
#include <fstream>
#include <iostream>
#include <set>
#include <sstream>

#include <climits>
#include <unistd.h>

namespace cli {

namespace input {

std::string header;
std::string prolog_file, epilog_file, epilog_fwd_file;

std::map<std::string, std::string> additional_class_files;

} // namespace input

namespace interp {
std::string target_class;

std::vector<std::string> includes;
std::vector<std::string> defines;
} // namespace interp

namespace output {

std::string file, dir, path;

bool order_alphabetically = false;
bool gen_flat = false;
bool emit_python = false;

} // namespace output

void Usage(char const *argv[]) {
  fmt::print(
      R"([USAGE] {}  -i <header_file> -t <classname> -o <filename_stub> [args]

Required arguments:
  -i|--input <header_file>       : The C++ header file that defines the class
  -t|--target <classname>        : The class to generate a proxy for
  -o|--output <filename stub>    : Output filename stub

Optional arguments:
  -I <path>                      : A directory to add to the include path
  -D <symbol>[=val]              : A symbol definition, with optional value, to the interpreter before parsing
  --extra-cflags <flags>         : A compatibility option with castxml-based SRProxy. Ignores passed compiler flags
                                     and picks up any -D<> and -I<> style flags included.

  --flat                         : Generate a 'flat' file reader rather than the objectified proxy class

  --order-alphabetically         : Emit datamembers in alphabetic, rather than declaration, order.

  -p|--include-path <path1[:p2]> : A PATH-like colon-separate list of directories to add to the include path
  -op|--output-path <path>       : A path to prepend to include statements in generated headers
  -od|--output-dir <path>        : The directory to write generated files to
  --prolog <file path>           : A file to include before the generated proxy class defintion
  --epilog <file path>           : A file to include after the generated proxy class definition
  --epilog-fwd <file path>       : A file to include after the list of generated forward declarations
  --extra <classname> <file>     : A file to include in the definition of the proxy class for class <classname>

  --emit-python-bindings         : Write pybind11 python bindings to <-o>.pybind.cxx

  -v|--verbose                   : Be louder
  -vv|--vverbose                 : Be even louder
  -h|-?|--help                   : Print this message
)",
      argv[0]);
}

std::vector<std::string> arg_buffer;

void ParseOpts(int argc, char const *argv[]) {
  arg_buffer = {
      argv[0],
  };

  // split up -Dsymbol and -I/path/ style compiler flags so that the parser
  // below can be homogeneous while also accepting standard format of compiler
  // flags
  for (int opt_it = 1; opt_it < argc; opt_it++) {
    std::string arg = argv[opt_it];
    if (arg.length() > 2) {
      if (arg.substr(0, 2) == "-D") {
        arg_buffer.push_back("-D");
        arg_buffer.push_back(arg.substr(2));
      } else if (arg.substr(0, 2) == "-I") {
        arg_buffer.push_back("-I");
        arg_buffer.push_back(arg.substr(2));
      } else if (arg.substr(0, 2) == "-f") {
        std::cout << "[WARNING]: Ignoring passed compiler-style flag " << arg
                  << ". \n[WARNING]: cling cannot accept arbitrary compiler "
                     "flags, but it shouldn't need to. Check if you really need"
                     " this to parse (n.b. not compile or link) the SRProxy "
                     "headers."
                  << std::endl;
      } else {
        arg_buffer.push_back(arg);
      }
    } else {
      arg_buffer.push_back(arg);
    }
  }

  for (int opt_it = 1; opt_it < arg_buffer.size(); opt_it++) {
    std::string arg = arg_buffer[opt_it];
    if ((arg == "-h") || (arg == "-?") || (arg == "--help")) {
      Usage(argv);
      exit(0);
    }

    if (arg == "--flat") {
      output::gen_flat = true;
      continue;
    } else if ((arg == "-v") || (arg == "--verbose")) {
      spdlog::set_level(spdlog::level::debug);
      continue;
    } else if ((arg == "-vv") || (arg == "--vverbose")) {
      spdlog::set_level(spdlog::level::trace);
      continue;
    } else if (arg == "--order-alphabetically") {
      output::order_alphabetically = true;
      continue;
    } else if (arg == "--emit-python-bindings") {
      output::emit_python = true;
      continue;
    }

    if ((opt_it + 1) < arg_buffer.size()) {
      if ((arg == "-i") || (arg == "--input")) {
        input::header = arg_buffer[++opt_it];
        continue;
      } else if ((arg == "-t") || (arg == "--target")) {
        interp::target_class = arg_buffer[++opt_it];
        continue;
      } else if ((arg == "-o") || (arg == "--output")) {
        output::file = arg_buffer[++opt_it];
        continue;
      } else if ((arg == "--extra-cflags")) {
        // skip this option as it only exists for compatibility with the old CLI
        continue;
      } else if ((arg == "-op") || (arg == "--output-path")) {
        output::path = arg_buffer[++opt_it];
        if (output::path.size() && (output::path.back() != '/')) {
          output::path += "/";
        }
        continue;
      } else if ((arg == "-od") || (arg == "--output-dir")) {
        output::dir = arg_buffer[++opt_it];
        if (output::dir.size() && (output::dir.back() != '/')) {
          output::dir += "/";
        }
        continue;
      } else if (arg == "--prolog") {
        input::prolog_file = arg_buffer[++opt_it];
        continue;
      } else if (arg == "--epilog") {
        input::epilog_file = arg_buffer[++opt_it];
        continue;
      } else if (arg == "--epilog-fwd") {
        input::epilog_fwd_file = arg_buffer[++opt_it];
        continue;
      } else if (arg == "-I") {
        interp::includes.push_back(arg_buffer[++opt_it]);
        continue;
      } else if (arg == "-D") {
        interp::defines.push_back(arg_buffer[++opt_it]);
        continue;
      } else if ((arg == "-p") || (arg == "--include-path")) {

        std::string ipath = arg_buffer[++opt_it];
        size_t colon = ipath.find_first_of(':');
        while (colon != std::string::npos) {
          if (colon != 0) {
            interp::includes.push_back(ipath.substr(0, colon));
          }
          ipath = ipath.substr(colon + 1, std::string::npos);
          colon = ipath.find_first_of(':');
        }

        if (ipath.size()) {
          interp::includes.push_back(ipath);
        }
        continue;
      }
    }

    if ((opt_it + 2) < arg_buffer.size()) {
      if (arg == "--extra") {
        std::string classname = arg_buffer[++opt_it];
        std::string deffile = arg_buffer[++opt_it];
        input::additional_class_files[classname] = deffile;
        continue;
      }
    }

    std::cout
        << "[ERROR]: Unknown option, or incorrect number of arguments for \""
        << arg << "\"" << std::endl;

    Usage(argv);
    exit(1);
  }
  if (!input::header.length() || !interp::target_class.length() ||
      !output::file.length()) {
    std::cerr
        << "[ERROR]: Not all required options recieved: (-i, -t, -o, -op)."
        << std::endl;
    Usage(argv);
    exit(1);
  }
}
} // namespace cli

namespace typeutils {

bool IsSTLVector(TDataMember &dm) {
  return (dm.IsSTLContainer() == TDictionary::kVector);
}

bool IsSTLVector(TClass *cls) {
  return (cls->GetCollectionType() == ROOT::kSTLvector);
}

bool IsStaticDatamember(TDataMember &dm, TClass *cls) {
  return (dm.GetOffset() > cls->GetClassSize());
}

bool KnownType(std::string name) {
  return gInterpreter->TypeInfo_IsValid(
      gInterpreter->TypeInfo_Factory(name.c_str()));
}

bool KnownClass(std::string name) {
  if ((name == "string") || (name == "std::string")) {
    return false; // pretend string is a primitive
  }
  return gInterpreter->ClassInfo_IsValid(
      gInterpreter->ClassInfo_Factory(name.c_str()));
}

std::string GetVectorValueTypeName(std::string classname) {
  auto openb = classname.find_first_of('<');
  auto closeb = classname.find_last_of('>');
  return classname.substr(openb + 1, closeb - openb - 1);
}

bool IsStandardTypeOrEnum(TDataMember &dm) {
  return (dm.IsBasic() || dm.IsEnum() ||
          (std::string(dm.GetTypeName()) == "string"));
}

std::string QualifystdNS(std::string classname) {
  for (std::string const &stype : {"vector", "string"}) {
    size_t pos = classname.find(stype);
    while (pos != std::string::npos) {

      if (pos && (classname[pos - 1] != ':')) {
        classname.replace(pos, 6, std::string("std::") + stype);
      } else if (!pos) {
        classname.replace(pos, 6, std::string("std::") + stype);
      }

      pos = classname.find(stype, pos + 6);
    }
  }
  return classname;
}

std::string GetTypeName(TDataMember &dm) {
  std::stringstream tn;
  tn << dm.GetTypeName();

  if (dm.GetArrayDim()) {
    tn << " ";
  }

  for (int i = 0; i < dm.GetArrayDim(); ++i) {
    tn << "[" << dm.GetMaxIndex(i) << "]";
  }

  std::string name = QualifystdNS(tn.str());

  // remove C++03 spaces between angle-brackets
  size_t pos = name.find("> >");
  while (pos != std::string::npos) {
    name.replace(pos, 3, ">>");
    pos = name.find("> >");
  }

  return name;
}

std::string GetNS(std::string classname) {
  size_t lpos = classname.rfind("::");

  if (lpos != std::string::npos) {
    return classname.substr(0, lpos);
  }
  return "";
}

std::string GetClassName(std::string classname) {
  size_t lpos = classname.rfind("::");
  if (lpos != std::string::npos) {
    return classname.substr(lpos + 2, std::string::npos);
  }
  return classname;
}

std::string GetShortProxyType(std::string classname) {
  return GetClassName(classname) + "Proxy";
}

std::string GetShortFlatType(std::string classname) {
  return std::string("Flat") + GetClassName(classname);
}

std::string GetShortType(std::string classname) {
  return cli::output::gen_flat ? GetShortFlatType(classname)
                               : GetShortProxyType(classname);
}

std::string GetPythonClassName(std::string classname) {
  for (auto &c : classname) {
    if ((c == ':')) {
      c = '_';
    }
    if ((c == '<')) {
      c = 'L';
    }
    if ((c == '>')) {
      c = 'R';
    }
  }
  return classname;
}

} // namespace typeutils

std::set<std::string> types_to_proxy;

std::string indent = "";

void WalkClass(TClass *cls) {
  if (!cls) {
    spdlog::error("WalkClass was passed a nullptr.");
    abort();
  }

  // We have already walked this type
  if (types_to_proxy.count(cls->GetName())) {
    spdlog::trace("{}Already known class: \"{}\"", indent, cls->GetName());
    return;
  }

  // this enables datamembers that are vectors of
  // vectors to be properly processed
  if (typeutils::IsSTLVector(cls)) {
    spdlog::trace("{}Found STL Vector type: \"{}\"", indent, cls->GetName());
    auto vvt = typeutils::GetVectorValueTypeName(cls->GetName());
    spdlog::trace("{}Determined value type as: \"{}\"", indent, vvt);

    if (!typeutils::KnownType(vvt)) {
      spdlog::error("TCling has no typeinfo for {}", vvt);
      abort();
    }

    // If the contained type is a class (as opposed to a
    // primitive), then we should check that we know how
    // to proxy the vector value type
    if (typeutils::KnownClass(vvt)) {
      if (gInterpreter->ClassInfo_IsEnum(vvt.c_str())) {
        types_to_proxy.insert(vvt);
      } else {
        indent += "- ";
        spdlog::debug("{}Walking RTTI tree for class: \"{}\"", indent, vvt);
        WalkClass(TClass::GetClass(vvt.c_str()));
        indent = indent.substr(0, indent.size() - 2);
      }
    }

    // We don't need to emit a proxy class for the vector template itself
    return;
  }

  spdlog::debug("{}Registering known class: \"{}\"", indent, cls->GetName());
  types_to_proxy.insert(cls->GetName());

  spdlog::trace("{}Class {}, has {} base classes.", indent, cls->GetName(),
                cls->GetListOfBases()->GetEntries());

  if (cls->GetListOfBases()->GetEntries() > 1) {
    spdlog::error("Class {} has {}  base classes, but we can currently only "
                  "handle single inheritance.",
                  cls->GetName(), cls->GetListOfBases()->GetEntries());
    abort();
  }

  for (auto base_to : *cls->GetListOfBases()) {
    auto bcls = dynamic_cast<TBaseClass *>(base_to)->GetClassPointer();
    indent += "- ";
    spdlog::debug("{}Walking RTTI tree for base class: \"{}\"", indent,
                  bcls->GetName());
    WalkClass(bcls);
    indent = indent.substr(0, indent.size() - 2);
  }

  for (auto pm_to : *cls->GetListOfAllPublicMethods()) {

    auto pm_ptr = dynamic_cast<TMethod *>(pm_to);
    if (pm_ptr->GetName()[0] == '~') { // skip destructors
      continue;
    }
    spdlog::trace("{}Examining public function: \"{}\" with {} arguments",
                  indent, pm_ptr->GetName(),
                  pm_ptr->GetListOfMethodArgs()->GetEntries());

    indent += "| ";
    for (auto ma_to : *pm_ptr->GetListOfMethodArgs()) {
      auto ma = dynamic_cast<TMethodArg const *>(ma_to);

      if (!ma) {
        spdlog::error("Failed to resolve TMethodArg {}", ma_to->GetName());
        abort();
      }

      spdlog::trace("{}Examining public method argument of type \"{}\"", indent,
                    ma->GetTypeName());

      if (gInterpreter->ClassInfo_IsEnum(ma->GetTypeName())) {
        types_to_proxy.insert(ma->GetTypeName());
      } else {
        auto *acls = TClass::GetClass(ma->GetTypeName());
        if (acls != cls) { // skip arguments of yourself
          indent += "- ";
          spdlog::debug("{}Walking RTTI tree for method argument class: \"{}\"",
                        indent, acls->GetName());
          WalkClass(acls);
          indent = indent.substr(0, indent.size() - 2);
        }
      }
    }
    indent = indent.substr(0, indent.size() - 2);
  }

  // Loop through this classes public data members, and method arguments
  // checking if we need to emit proxies for any of their types
  std::vector<TDataMember *> DataMembers;

  for (auto dm_to : *cls->GetListOfAllPublicDataMembers()) {

    auto dm_ptr = dynamic_cast<TDataMember *>(dm_to);

    if (!dm_ptr->IsValid()) {
      spdlog::error("Failed to read type for data member {} of class {} ",
                    dm_ptr->GetName(), cls->GetName());
      abort();
    }

    DataMembers.push_back(dm_ptr);
  }

  // The pygccxml/castxml version traversed data members alphabetically rather
  // than in declaration order.
  if (cli::output::order_alphabetically) {
    std::sort(DataMembers.begin(), DataMembers.end(),
              [](TDataMember const *l, TDataMember const *r) {
                return std::string(l->GetName()).compare(r->GetName()) < 0;
              });
  }

  for (auto dm_ptr : DataMembers) {
    auto &dm = *dm_ptr;

    spdlog::trace(
        "{}Examining data member: \"{}\" of type {} (Basic: {}, Enum: {})",
        indent, dm.GetName(), dm.GetTypeName(),
        (typeutils::IsStandardTypeOrEnum(dm) ? "true" : "false"),
        (dm.IsEnum() ? "true" : "false"));

    // If this data member's type is not a primitive, then we need to check if
    // we need to emit a proxy class for it
    if (!typeutils::IsStandardTypeOrEnum(dm)) {
      indent += "- ";
      spdlog::debug("{}Walking RTTI tree for class: \"{}\"", indent,
                    dm.GetTypeName());
      WalkClass(TClass::GetClass(dm.GetTypeName()));
      indent = indent.substr(0, indent.size() - 2);
    } else if (dm.IsEnum()) {

      // Add this type to the list of types if we don't already know about it
      if (!types_to_proxy.count(dm.GetTypeName())) {
        spdlog::debug("{}Storing declaration of enum: \"{}\"", indent,
                      dm.GetTypeName());
        types_to_proxy.insert(dm.GetTypeName());
      }
    }
  }
}

bool PrepareInterpreter() {
  // these "helpful" behaviors of TCling
  // are anything but
  gInterpreter->SetClassAutoloading(false);
  gInterpreter->SetClassAutoparsing(false);

  for (auto const &ip : cli::interp::includes) {
    spdlog::debug("Adding include path: \"{}\"", ip);

    gInterpreter->AddIncludePath(ip.c_str());
  }

  for (auto def : cli::interp::defines) {
    spdlog::debug("Adding symbol definition: \"{}\"", def);

    auto eq_pos = def.find_first_of('=');
    if (eq_pos != std::string::npos) {
      std::string mdef = def;
      mdef[eq_pos] = ' ';
      spdlog::debug("  \"{}\" => \"{}\"", def, mdef);
      def[eq_pos] = ' ';
    }

    gInterpreter->LoadText(fmt::format("#define {}", def).c_str());
  }

  spdlog::debug("Interpreting header: \"{}\"", cli::input::header);

  if (!gInterpreter->LoadText(fmt::format("#include \"{}\"", cli::input::header)
                                  .c_str())) { // returns int(true) on failure
    spdlog::error("TCling failed read: {}", cli::input::header);
    return false;
  }
  return true;
}

struct {
  std::string prolog_contents;
  std::string epilog_contents;
  std::string epilog_fwd_contents;

  std::map<std::string, std::pair<std::string, bool>>
      additional_class_defintions;
} extra_code;

std::string ReadFileContents(std::string const &fn) {
  std::ifstream fs(fn.c_str());
  if (!fs.is_open()) {
    spdlog::error("Failed to read input file: {} ", fn);
    abort();
  }
  std::stringstream ss;
  ss << fs.rdbuf();
  return ss.str();
}

void ReadExtras() {
  for (auto const &acf : cli::input::additional_class_files) {
    spdlog::debug("Loading additional implementation file: \"{}\" for class {}",
                  acf.second, acf.first);
    extra_code.additional_class_defintions[acf.first] =
        std::make_pair(ReadFileContents(acf.second), false);
  }

  if (cli::input::prolog_file.size()) {
    spdlog::debug("Reading prolog file: \"{}\"", cli::input::prolog_file);
    extra_code.prolog_contents = ReadFileContents(cli::input::prolog_file);
  }

  if (cli::input::epilog_file.size()) {
    spdlog::debug("Reading epilog file: \"{}\"", cli::input::epilog_file);
    extra_code.epilog_contents = ReadFileContents(cli::input::epilog_file);
  }

  if (cli::input::epilog_fwd_file.size()) {
    spdlog::debug("Reading epilog fwd file: \"{}\"",
                  cli::input::epilog_fwd_file);
    extra_code.epilog_fwd_contents =
        ReadFileContents(cli::input::epilog_fwd_file);
  }
}

std::string QualifyDisclaimer() {
  //   SRProxy Verion: {0}
  //   datetime: {1}
  //   host: {2}
  //   command: {3}
  std::stringstream command_buffer;
  for (int i = 0; i < cli::arg_buffer.size(); ++i) {
    command_buffer << cli::arg_buffer[i]
                   << ((i + 1 != cli::arg_buffer.size()) ? " " : "");
  }

  std::stringstream generator_host;
  char hostname[HOST_NAME_MAX];
  char username[LOGIN_NAME_MAX];
  bool have_user = false;
  if (!getlogin_r(username, LOGIN_NAME_MAX)) {
    generator_host << username;
    have_user = true;
  }
  if (!gethostname(hostname, HOST_NAME_MAX)) {
    generator_host << (have_user ? "@" : "") << hostname;
  }

  return fmt::format(tmplt::disclaimer, SRProxy_VERSION, BUILD_ROOT_VERSION,
                     BUILD_ROOT_LIBRARY_DIR, fmt::gmtime(std::time(nullptr)),
                     generator_host.str(), command_buffer.str());
}

std::string CutSStream(std::stringstream const &ss, size_t n) {
  std::string rtn = ss.str();
  return rtn.substr(0, rtn.length() - n);
}

struct EmittedCode {
  std::string hdr, impl, fwd, pyb;
};

EmittedCode EmitClass(std::string classname) {

  auto const &templates = cli::output::gen_flat ? tmplt::flat : tmplt::proxy;

  std::stringstream ss_hdr, ss_impl, ss_fwd, ss_pyb;

  EmittedCode code;

  std::stringstream inits;
  std::stringstream memberlist;

  std::stringstream memberlist_pyimpl;

  std::stringstream assign_body;
  std::stringstream checkequals_body;

  std::stringstream fill_body;
  std::stringstream clear_body;

  auto cls = TClass::GetClass(classname.c_str());

  std::string type = classname;
  std::string typename_noNS = typeutils::GetClassName(classname);
  std::string ptype = fmt::format("{}<{}>", templates.prefix, classname);

  std::string base_declaration;
  std::set<std::string> base_members;

  size_t nbases = 0;
  for (auto base_to : *cls->GetListOfBases()) {
    auto bcls = dynamic_cast<TBaseClass *>(base_to)->GetClassPointer();

    std::string pbtype =
        fmt::format("{}<{}>", templates.prefix, bcls->GetName());

    inits << fmt::format(templates.base_init, pbtype);
    assign_body << fmt::format(tmplt::assign_base_body, pbtype);
    checkequals_body << fmt::format(tmplt::checkequals_base_body, pbtype);

    fill_body << fmt::format(tmplt::fill_base_body, pbtype);
    clear_body << fmt::format(tmplt::clear_base_body, pbtype);

    base_declaration = fmt::format(" : public {}", pbtype);

    for (auto dm_to : *bcls->GetListOfAllPublicDataMembers()) {
      auto &dm = dynamic_cast<TDataMember &>(*dm_to);
      base_members.insert(dm.GetName());
    }

    nbases++;
  }

  // base classes need their Proxies to inherit from Lineage
  if (!cli::output::gen_flat && (nbases == 0)) {
    inits << " Lineage(parent),\n";
    base_declaration = " : public caf::Lineage";
  }

  std::vector<TDataMember *> DataMembers;
  std::vector<std::string> vector_types;

  for (auto dm_to : *cls->GetListOfAllPublicDataMembers()) {

    auto dm_ptr = dynamic_cast<TDataMember *>(dm_to);

    if (!dm_ptr->IsValid()) {
      spdlog::error("Failed to read type for data member {} of class {} ",
                    dm_ptr->GetName(), cls->GetName());
      abort();
    }

    DataMembers.push_back(dm_ptr);
  }

  // The pygccxml/castxml version traversed data members alphabetically
  // rather than in declaration order.
  if (cli::output::order_alphabetically) {
    std::sort(DataMembers.begin(), DataMembers.end(),
              [](TDataMember const *l, TDataMember const *r) {
                return std::string(l->GetName()).compare(r->GetName()) < 0;
              });
  }

  for (auto dm_ptr : DataMembers) {
    auto &dm = *dm_ptr;

    std::string mname = dm.GetName();

    if (base_members.count(
            mname)) { // Don't re-declare members of the base class
      continue;
    }

    // Check if the data member is static and skip if it is
    if (typeutils::IsStaticDatamember(dm, cls)) {
      continue;
    }
    std::string mptype =
        fmt::format("{}<{}>", templates.prefix, typeutils::GetTypeName(dm));

    inits << fmt::format(templates.member_init, mname);

    memberlist << fmt::format(tmplt::member_list, mptype, mname);

    // if (emit_python) {
    //   if (!typeutils::IsStandardTypeOrEnum(dm) || dm.GetArrayDim()) {
    //     if (typeutils::IsSTLVector(dm)) {
    //       vector_types.push_back(GetTypeName(dm));
    //     }
    //     memberlist_pyimpl << fmt::format(tmplt::python::datamember_proxy,
    //     mname,
    //                                      classname, GetTypeName(dm));

    //   } else {
    //     memberlist_pyimpl <<
    //     fmt::format(tmplt::python::datamember_basic_type,
    //                                      mname, classname,
    //                                      GetTypeName(dm));
    //   }
    // }

    assign_body << fmt::format(tmplt::assign_member_body, mname);
    checkequals_body << fmt::format(tmplt::checkequals_member_body, mname);

    fill_body << fmt::format(tmplt::fill_member_body, mname);
    clear_body << fmt::format(tmplt::clear_member_body, mname);
  }

  //{0} == Namespace
  //{1} == Type
  //{2} == ShortType
  //{3} == ProxyType
  ss_fwd << fmt::format(tmplt::fwd_body, typeutils::GetNS(classname),
                        typename_noNS, typeutils::GetShortType(classname),
                        fmt::format("{}<{}>", templates.prefix, classname));

  //{0} == Type
  //{1} == ProxyType
  //{2} == BaseClass
  //{3} == AdditionalClasses
  //{4} == Members
  std::string additional_definitions = "";
  if (extra_code.additional_class_defintions.count(typename_noNS)) {
    extra_code.additional_class_defintions[typename_noNS].second = true;
    additional_definitions =
        extra_code.additional_class_defintions[typename_noNS].first;
  }

  ss_hdr << fmt::format(templates.hdr_body, type, ptype, base_declaration,
                        additional_definitions, CutSStream(memberlist, 1));

  //{0} == ProxyType
  //{1} == Inits
  //{2} == Type
  //{3} == AssignBody
  //{4} == CheckEqualsBody
  ss_impl << fmt::format(
      templates.cxx_body, ptype,
      // Trim off the last newline and comma from this list
      CutSStream(inits, 2), type,
      CutSStream(cli::output::gen_flat ? fill_body : assign_body, 1),
      CutSStream(cli::output::gen_flat ? clear_body : checkequals_body, 1));

  // if (emit_python) {

  //   for (auto const &vector_type : vector_types) {
  //     if (!py_emitted_vector_types.count(vector_type)) {

  //       auto vvt = typeutils::GetVectorValueTypeName(vector_type);
  //       if (typeutils::KnownClass(vvt)) {
  //         out_pyb << fmt::format(tmplt::python::vector_of_proxies,
  //         vector_type,
  //                                typeutils::GetPythonClassName(vector_type),
  //                                vvt);
  //       } else { // builtin type that we can just return rather than
  //       returning
  //                // the proxy
  //         out_pyb << fmt::format(tmplt::python::vector_of_basic_types,
  //                                vector_type,
  //                                typeutils::GetPythonClassName(vector_type));
  //       }

  //       py_emitted_vector_types.insert(vector_type);
  //     }
  //   }

  //   out_pyb << fmt::format(tmplt::python::class_declaration, classname,
  //                          typeutils::GetPythonClassName(classname));
  //   out_pyb << memberlist_pyimpl.str() << "\n;";
  // }
  return {ss_hdr.str(), ss_impl.str(), ss_fwd.str(), ss_pyb.str()};
}

void WriteOutput() {

  auto const &templates = cli::output::gen_flat ? tmplt::flat : tmplt::proxy;

  std::stringstream ss_hdr;
  std::stringstream ss_impl;
  std::stringstream ss_fwd;
  std::stringstream ss_pyb;

  auto qualified_disclaimer = QualifyDisclaimer();

  ss_hdr << qualified_disclaimer;
  ss_impl << qualified_disclaimer;
  ss_fwd << qualified_disclaimer;

  ss_fwd << templates.fwd_prolog;

  //{0} == Prolog
  //{1} == Outpath
  ss_hdr << fmt::format(templates.hdr_prolog, extra_code.prolog_contents,
                        cli::output::path);

  //{0} == Header
  //{1} == Input
  ss_impl << fmt::format(
      templates.cxx_prolog,
      fmt::format("{}{}.h", cli::output::path, cli::output::file),
      cli::input::header);

  for (auto tname : types_to_proxy) {
    if (gInterpreter->ClassInfo_IsEnum(tname.c_str())) {
      spdlog::debug("Emitting explicit template instantiation for enum: \"{}\"",
                    tname);

      ss_impl << fmt::format("template class {}<{}>;\n", templates.prefix,
                             tname);
    } else {

      spdlog::debug("Emitting proxy for class: \"{}\"", tname);

      auto const &[hdr, impl, fwd, pyb] = EmitClass(tname);
      ss_hdr << hdr;
      ss_impl << impl;
      ss_fwd << fwd;
      ss_pyb << pyb;
    }
  }

  std::ofstream out_hdr(cli::output::dir + cli::output::file + ".h");
  out_hdr << ss_hdr.str();
  std::ofstream out_impl(cli::output::dir + cli::output::file + ".cxx");
  out_impl << ss_impl.str();
  std::ofstream out_fwd(cli::output::dir + "FwdDeclare.h");
  out_fwd << ss_fwd.str();

  if (cli::output::emit_python) {
    // std::unique_ptr<std::ofstream> out_pyb;
    // if (cli::output::emit_python) {

    //   (*out_pyb) << fmt::format(tmplt::python::impl_frontmatter,
    //                             cli::input::header, cli::output::file);

    //   (*out_pyb) << tmplt::python::lineage_ancestor_type_cppdeclaration;
    //   int enumid = 0;
    //   for (auto classname : Declarations) {
    //     (*out_pyb) << fmt::format(tmplt::python::lineage_ancestor_cpptype,
    //                               typeutils::GetPythonClassName(classname),
    //                               enumid++);
    //   }
    //   (*out_pyb) << R"(
    // };
    // )";

    //   (*out_pyb) << fmt::format(tmplt::python::module_declaration,
    //                             cli::output::file);
    // }
    // if (cli::output::emit_python) { // build the Proxied class type enum
    //   for
    //     use
    //         // with
    //         // Lineage::Ancestor on the python side

    //         std::stringstream pyenumss,
    //         pyancestorss;

    //   pyenumss << tmplt::python::lineage_ancestor_type_pydeclaration;
    //   pyancestorss << tmplt::python::lineage_ancestor_function;
    //   int enumid = 0;
    //   for (auto classname : Declarations) {
    //     pyenumss << fmt::format(tmplt::python::lineage_ancestor_pytype,
    //                             typeutils::GetPythonClassName(classname));
    //     pyancestorss << fmt::format(tmplt::python::lineage_ancestor_case,
    //                                 typeutils::GetPythonClassName(classname),
    //                                 classname);
    //   }
    //   pyenumss << tmplt::python::lineage_ancestor_type_pyfinalize;
    //   pyancestorss << tmplt::python::lineage_default_rvp;

    //   (*out_pyb) << pyenumss.str() << pyancestorss.str();
    // }
    std::ofstream out_pyb(cli::output::dir + cli::output::file + ".pybind.cxx");
    out_pyb << ss_pyb.str();
    out_pyb << fmt::format(tmplt::python::proxyfilereader,
                           cli::interp::target_class,
                           typeutils::GetClassName(cli::interp::target_class));

    out_pyb << "}\n";
  }

  if (extra_code.epilog_contents.size()) {
    spdlog::debug("Writing epilog");
    out_hdr << extra_code.epilog_contents;
  }
  if (extra_code.epilog_fwd_contents.size()) {
    spdlog::debug("Writing epilog for fwd declare");

    out_fwd << extra_code.epilog_fwd_contents;
  }
}

int main(int argc, char const *argv[]) {
  cli::ParseOpts(argc, argv);
  spdlog::set_pattern("%v");

  if (!PrepareInterpreter()) {
    return 1;
  }
  ReadExtras();

  spdlog::debug("Requesting RTTI for class: \"{}\"", cli::interp::target_class);

  auto tcls = TClass::GetClass(cli::interp::target_class.c_str());

  if (!tcls) {
    spdlog::error("TCling failed to find class: {} declaration in: {}",
                  cli::interp::target_class, cli::input::header);
    return 2;
  }

  std::vector<std::string> Declarations;
  std::vector<std::string> EDeclarations;
  spdlog::debug("Walking RTTI tree for class: \"{}\"",
                cli::interp::target_class);
  indent = "+ ";
  WalkClass(tcls);

  WriteOutput();

  for (auto const &[classname, cu] : extra_code.additional_class_defintions) {
    if (!cu.second) {
      spdlog::warn("--extra class argument: {} was not used.", classname);
    }
  }
}
