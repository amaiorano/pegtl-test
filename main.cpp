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

    // Primitive type def
    struct type_def_name : plus<seq<identifier, blanks>> {};
    struct type_def_id : digits {};
    struct type_def_range_def_id : digits {};
    struct type_def_range_lower_bound : digits {};
    struct type_def_range_upper_bound : digits {};
    struct type_def_range
        : seq<one<'='>, one<'r', 'R'>, type_def_range_def_id, one<';'>, type_def_range_lower_bound,
              one<';'>, type_def_range_upper_bound, one<';'>> {};
    struct type_def : seq<type_def_name, one<':'>, one<'t'>, type_def_id, opt<type_def_range>> {};

    // Variable decl and pointer def
    // a:7
    // p:25=*7
    struct pointer_def_id : digits {};
    struct pointer_ref_id : digits {};
    struct pointer_def : seq<pointer_def_id, one<'='>, one<'*'>, pointer_ref_id> {};
    struct type_ref_id : digits {};
    struct type_ref : sor<pointer_def, type_ref_id> {};
    struct variable_name : identifier {};
    struct variable : seq<variable_name, one<':'>, type_ref> {};

    // Array type def and variable decl
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
    struct array_type_id : digits {};
    struct array_max_index : digits {};
    // 25=ar26=r26;0;-1;;0;9;
    // 27=ar26;0;10;
    // 28=ar26;0;11;
    struct array_type : seq<array_type_id, string<'=', 'a', 'r'>, digits, opt<array_subrange>,
                            one<';'>, digits, one<';'>, array_max_index, one<';'>> {};
    // 7
    struct terminal_array_type : seq<type_ref> {};
    struct array : seq<array_name, one<':'>, plus<array_type>, terminal_array_type> {};

    struct lsym : sor<array, type_def, variable> {};

    struct param_string : seq<dquote, lsym, dquote> {};
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

    // Types selected in for parse tree
    template <typename Rule>
    using selector =
        parse_tree::selector<Rule, pegtl::parse_tree::store_content::on<
                                       // array
                                       array, array_name, array_type_id, array_max_index,
                                       // type_def
                                       type_def, type_def_name, type_def_id,
                                       type_def_range_lower_bound, type_def_range_upper_bound,
                                       // variable
                                       variable, type_ref, variable_name, type_ref_id, pointer_def,
                                       pointer_def_id, pointer_ref_id>>;

    using Node = pegtl::parse_tree::node;

    void PrintParseTree(const Node& node, int depth = 0) {
        std::function<void(const Node&, int)> f;
        f = [&f](const Node& node, int depth) {
            for (auto d = depth; d-- > 0;)
                std::cout << " ";
            std::cout << node.type << ": " << node.string_view() << std::endl;
            for (auto& c : node.children) {
                f(*c, depth + 1);
            }
        };

        assert(node.is_root()); // Root is the only node with no content
        for (auto& c : node.children) {
            f(*c, 0);
        }
        std::cout << std::endl;
    }

} // namespace stabs

int main() {
    const std::size_t issues = tao::pegtl::analyze<stabs::grammar>();

    // clang-format off
    std::vector<const char*> source = {
        //R"(                            204 ;	.stabs	"src/vectrexy.h",132,0,0,Ltext2)",
        //R"(                            206 ;    .stabd	68, 0, 61)",

        //R"(                             31 ;	.stabs	"complex long double:t3=R3;8;0;",128,0,0,0)",
        //R"(                            162 ;	.stabs	"a:7",160,0,0,0)",
        //R"(                             40 ;	.stabs	"int:t7",128,0,0,0)",
        //R"(                             41 ;	.stabs	"char char:t13=r13;0;255;",128,0,0,0)",
        //R"(                             31 ;	.stabs	"complex long double:t3=R3;8;0;",128,0,0,0)",
        //R"(                            162 ;	.stabs	"b:7",160,0,0,0)",
        //R"(                             86 ;	.stabs	"c:25=ar26=r26;0;-1;;0;9;27=ar26;0;10;28=ar26;0;11;7",128,0,0,0)",

        
        R"(                            167;.stabs	"a:7",128,0,0,7)",
        R"(                            168;.stabs	"p:25=*7",128,0,0,5)",
        R"(                            132;.stabs	"b:30=ar28;0;2;22",128,0,0,18)",
        R"(                            133;.stabs	"pi:31=ar28;0;3;32=*7",128,0,0,10)",


        //R"(                            169;.stabs	"p2:25",128,0,0,8)",
        //R"(                            170;.stabs	"r3:26=*7",128,0,0,10)",
        //R"(                            171;.stabs	"r4:26",128,0,0,12)",
        //R"(                            172;.stabs	"pp:27=*25",128,0,0,3)",
        //R"(                            173;.stabs	"ppp:28=*27",128,0,0,1)",
        //R"(                            174;.stabs	"rppp:29=*28",128,0,0,14)",




    };
    // clang-format on

    for (auto s : source) {
        // pegtl::standard_trace<stabs::grammar>(pegtl::string_input(s, "stabs source"));
        pegtl::string_input in(s, "stabs source");
        if (const auto root =
                pegtl::parse_tree::parse<stabs::grammar, stabs::Node, stabs::selector>(in)) {
            // pegtl::parse_tree::print_dot(std::cout, *root);
            stabs::PrintParseTree(*root);
        }
    }
}
