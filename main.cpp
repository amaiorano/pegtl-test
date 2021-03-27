// Copyright (c) 2014-2021 Dr. Colin Hirsch and Daniel Frey
// Please see LICENSE for license or visit https://github.com/taocpp/PEGTL/

#include <functional>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <variant>

#include <tao/pegtl.hpp>
#include <tao/pegtl/contrib/analyze.hpp>
#include <tao/pegtl/contrib/parse_tree.hpp>
#include <tao/pegtl/contrib/trace.hpp>

namespace pegtl = TAO_PEGTL_NAMESPACE;

namespace {
    auto stoi(std::string_view sv) { return stoi(std::string{sv}); }
} // namespace

namespace stabs {
    using namespace pegtl;

    struct blanks : star<blank> {};
    struct digits : seq<opt<one<'-'>>, plus<digit>> {};
    struct dquote : one<'\"'> {};
    struct comma : one<','> {};
    struct unquoted_string : plus<alnum> {};
    struct dquoted_string : seq<dquote, plus<not_at<dquote>, any>, dquote> {};
    struct sep : seq<blanks, comma, blanks> {};

    // Match stabs type string for N_LSYM: type definitions or variable declarations
    // Type definitions:
    // "int:t7"
    // "char:t13=r13;0;255;"
    //
    // Local variables:
    // "a:7"
    //
    // 1: type/variable name
    // 2: 't' for type, or nothing if variable declaration
    // 3: type def/ref #
    // 4: type range, or nothing (full match)
    //  5: type-def # that this is a range of (can be self-referential)
    //  6: lower-bound of range (if > upper-bound, is size in bytes)
    //  7: upper-bound of range

    struct type_def_name : plus<seq<identifier, blanks>> {};
    struct type_def_num : digits {};
    struct type_def_range_def_num : digits {};
    struct type_def_range_lower_bound : digits {};
    struct type_def_range_upper_bound : digits {};
    struct type_def_range
        : seq<one<'='>, one<'r', 'R'>, type_def_range_def_num, one<';'>, type_def_range_lower_bound,
              one<';'>, type_def_range_upper_bound, one<';'>> {};
    struct type_def : seq<type_def_name, one<':'>, one<'t'>, type_def_num, opt<type_def_range>> {};

    struct type_ref_name : identifier {};
    struct type_ref_num : digits {};
    struct type_ref : seq<type_ref_name, one<':'>, type_ref_num> {};

    // Arrays
    // int c[10][11][12];
    //      .stabs	"c:25=ar26=r26;0;-1;;0;9;27=ar26;0;10;28=ar26;0;11;7",128,0,0,0
    //
    // int i[1];        i:25=ar26=r26;0;-1;;0;0;7
    // char c[2];       c:27=ar26;0;1;13
    // bool b[3];       b:28=ar26;0;2;22
    // int* pi[4];      pi:29=ar26;0;3;30=*7

    // Range of the array type num (we generally ignore this)
    // =r26;0;-1;
    struct array_subrange
        : seq<string<'=', 'r'>, digits, one<';'>, digits, one<';'>, digits, one<';'>> {};

    struct array_name : identifier {};
    struct array_type_num : digits {};
    struct array_max_index : digits {};
    // 25=ar26=r26;0;-1;;0;9;
    // 27=ar26;0;10;
    // 28=ar26;0;11;
    struct array_type : seq<array_type_num, string<'=', 'a', 'r'>, digits, opt<array_subrange>,
                            one<';'>, digits, one<';'>, array_max_index, one<';'>> {};
    // 7
    struct terminal_array_type : seq<type_ref_num> {
    }; // TODO: eventually support pointer defs too (27=*7)
    struct array : seq<array_name, one<':'>, plus<array_type>, terminal_array_type> {};

    struct lsym : sor<array, type_def, type_ref> {};

    template <typename Rule>
    using selector =
        parse_tree::selector<Rule, pegtl::parse_tree::store_content::on<
                                       // array
                                       array, array_name, array_type_num, array_max_index,
                                       // type_def
                                       type_def, type_def_name, type_def_num,
                                       type_def_range_lower_bound, type_def_range_upper_bound,
                                       // type_ref
                                       type_ref, type_ref_name, type_ref_num

                                       >>;

    struct param_string : seq<dquote, lsym, dquote> {}; // dquoted_string {};
    struct param_type : digits {};
    struct param_other : digits {};
    struct param_desc : digits {};
    struct param_value : unquoted_string {};

    struct str_stabs : TAO_PEGTL_STRING(".stabs") {};
    struct str_stabd : TAO_PEGTL_STRING(".stabd") {};

    struct stabs_directive_prefix : seq<star<not_at<str_stabs>, any>, str_stabs, blanks> {};
    struct stabd_directive_prefix : seq<star<not_at<str_stabd>, any>, str_stabd, blanks> {};

    // Match stabs (string) directive
    // Captures: 1:string, 2:type, 3:other, 4:desc, 5:value
    //    204 ;	.stabs	"src/vectrexy.h",132,0,0,Ltext2
    struct stabs_directive : seq<stabs_directive_prefix, param_string, sep, param_type, sep,
                                 param_other, sep, param_desc, sep, param_value> {};

    // Match stabd (dot) directive
    // Captures: 1:type, 2:other, 3:desc
    //    206;.stabd	68, 0, 61
    struct stabd_directive
        : seq<stabd_directive_prefix, param_type, sep, param_other, sep, param_desc> {};

    struct grammar : must<seq<sor<stabs_directive, stabd_directive>, eof>> {};

    // Public API
    struct TypeDef {
        struct Range {
            int num = ~0;
            int lowerBound = 0;
            int upperBound = 0;
        };

        std::string name;
        int num = ~0;
        std::optional<Range> range;
    };

    struct TypeRef {
        std::string name;
        int num = ~0;
    };

    // Private
    struct Callbacks {
        std::function<void(const TypeDef&)> onTypeDef;
        std::function<void(const TypeRef&)> onTypeRef;
    };

    class MatchHandler {
    public:
        MatchHandler(Callbacks& callbacks)
            : m_callbacks(callbacks) {}

        void type_def_name(std::string_view match) {
            m_curr = TypeDef{};
            As<TypeDef>().name = match;
        }
        void type_def_num(std::string_view match) { //
            As<TypeDef>().num = stoi(match);
        }
        void type_def_range_def_num(std::string_view match) {
            As<TypeDef>().range = {stoi(match), 0, 0};
        }
        void type_def_range_lower_bound(std::string_view match) {
            As<TypeDef>().range->lowerBound = stoi(match);
        }
        void type_def_range_upper_bound(std::string_view match) {
            As<TypeDef>().range->upperBound = stoi(match);
        }
        void type_def(std::string_view match) {
            m_callbacks.onTypeDef(As<TypeDef>());
            m_curr = Null{};
        }

        void type_ref_name(std::string_view match) {
            m_curr = TypeRef{};
            As<TypeRef>().name = match;
        }
        void type_ref_num(std::string_view match) { //
            As<TypeRef>().num = stoi(match);
        }
        void type_ref(std::string_view match) {
            m_callbacks.onTypeRef(As<TypeRef>());
            m_curr = Null{};
        }

    private:
        struct Null {};
        template <typename T>
        T& As() {
            return std::get<T>(m_curr);
        }

        Callbacks& m_callbacks;

        // TODO: stack of these for nested types
        std::variant<Null, TypeDef, TypeRef> m_curr = Null{};
    };

    template <typename Rule>
    struct action {};

#define ACTION_DEF(rule)                                                                           \
    template <>                                                                                    \
    struct action<rule> {                                                                          \
        template <typename ActionInput>                                                            \
        static void apply(const ActionInput& in, MatchHandler& v) {                                \
            v.##rule(in.string_view());                                                            \
        }                                                                                          \
    }

    ACTION_DEF(type_def_name);
    ACTION_DEF(type_def_num);
    ACTION_DEF(type_def_range_def_num);
    ACTION_DEF(type_def_range_lower_bound);
    ACTION_DEF(type_def_range_upper_bound);
    ACTION_DEF(type_def);

    ACTION_DEF(type_ref_name);
    ACTION_DEF(type_ref_num);
    ACTION_DEF(type_ref);

    // template <>
    // struct action<grammar> {
    //    template <typename ActionInput>
    //    static void apply(const ActionInput& in, std::string& v) {
    //        v = in.string();
    //    }
    //};

} // namespace stabs

int main() {
    const std::size_t issues = tao::pegtl::analyze<stabs::grammar>();

    // clang-format off
    std::vector<const char*> source = {
        //R"(                            204 ;	.stabs	"src/vectrexy.h",132,0,0,Ltext2)",
        //R"(                            206 ;    .stabd	68, 0, 61)",
        R"(                             31 ;	.stabs	"complex long double:t3=R3;8;0;",128,0,0,0)",
        //R"(                            162 ;	.stabs	"a:7",160,0,0,0)",
        //R"(                             40 ;	.stabs	"int:t7",128,0,0,0)",
        //R"(                             41 ;	.stabs	"char char:t13=r13;0;255;",128,0,0,0)",
        //R"(                             31 ;	.stabs	"complex long double:t3=R3;8;0;",128,0,0,0)",
        R"(                            162 ;	.stabs	"b:7",160,0,0,0)",

        R"(                             86 ;	.stabs	"c:25=ar26=r26;0;-1;;0;9;27=ar26;0;10;28=ar26;0;11;7",128,0,0,0)",
    };
    // clang-format on

    stabs::Callbacks callbacks;
    callbacks.onTypeDef = [](const stabs::TypeDef& typeDef) {
        std::cout << "onTypeDef: name=" << typeDef.name << " num=" << typeDef.num;
        if (auto r = typeDef.range) {
            std::cout << " range{num=" << r->num << " lowerBound=" << r->lowerBound
                      << " upperBound=" << r->upperBound << "}";
        }
        std::cout << std::endl;
    };
    callbacks.onTypeRef = [](const stabs::TypeRef& typeRef) {
        std::cout << "onTypeRef: name=" << typeRef.name << " num=" << typeRef.num << std::endl;
    };

    stabs::MatchHandler matchHandler(callbacks);

    for (auto s : source) {
        // pegtl::standard_trace<stabs::grammar>(pegtl::string_input(s, "stabs source"));
        pegtl::string_input in(s, "stabs source");

#if 1

        // struct Node : pegtl::parse_tree::basic_node<Node> {};
        using Node = pegtl::parse_tree::node;

        if (const auto root = pegtl::parse_tree::parse<stabs::grammar, Node, stabs::selector>(in)) {
            // pegtl::parse_tree::print_dot(std::cout, *root);

            std::function<void(const Node&, int)> f;
            f = [&f](const Node& node, int depth) {
                if (node.has_content()) {
                    for (auto d = depth; d-- > 0;)
                        std::cout << " ";
                    std::cout << node.type << ": " << node.string_view() << std::endl;
                }
                for (auto& c : node.children) {
                    f(*c, depth + 1);
                }
            };

            f(*root, 0);
        }

        std::cout << std::endl;
#endif

        // pegtl::string_input in2(s, "stabs source");
        // std::string output;
        // if (pegtl::parse<stabs::grammar, stabs::action>(in2, matchHandler)) {
        //    // std::cout << "\tOutput: " << output << std::endl;
        //    // std::cout << "Grammar matched" << std::endl;
        //} else {
        //    std::cerr << "I don't understand." << std::endl;
        //}
    }
}
