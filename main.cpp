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

    // Similar to until<R> except that it does not consume R
    template <typename RULE>
    struct until_not_at : star<not_at<RULE>, any> {};

    struct blanks : star<blank> {};
    struct digits : seq<opt<one<'-'>>, plus<digit>> {};
    struct dquote : one<'\"'> {};
    struct comma : one<','> {};
    struct unquoted_string : plus<alnum> {};
    struct dquoted_string : seq<dquote, until<dquote>> {};
    struct sep : seq<blanks, comma, blanks> {};
    struct file_path : star<sor<alnum, one<'-'>, one<'_'>, one<'/'>, one<'.'>>> {};

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

    // Match stabs type string for N_LSYM: enum type definitions
    // "bool:t22=eFalse:0,True:1,;"
    // "WeekDay:t25=eMonday:0,Tuesday:1,Wednesday:2,EndOfDays:2,Foo:-5000,;"
    //
    // 1: type (enum) name
    // 2: type def #
    // 3: values (comma-separated key:value pairs)
    struct enum_name : identifier {};
    struct enum_id : digits {};
    struct enum_value_id : identifier {};
    struct enum_value_num : digits {};
    struct enum_value : seq<enum_value_id, one<':'>, enum_value_num, comma> {};
    struct enum_
        : seq<enum_name, one<':'>, one<'t'>, enum_id, one<'='>, plus<enum_value>, one<';'>> {};

    // Match stabs type string for N_LSYM: struct/class type definitions
    // "Foo:T26=s4a:7,0,8;b:7,8,8;c:7,16,8;d:7,24,6;e:7,30,2;;
    //
    // 1: type name
    // 2: type def #
    // 3: total byte size of struct
    // 4: values (semicolon-separated key:value pairs)
    //  Splits out the array of values
    //  1: lsym string
    //  2: offset in bits
    //  3: size in bits
    //  "a:7,0,8;b:7,8,8;c:7,16,8;d:7,24,6;e:7,30,2;p:28=*7,88,16;"
    struct struct_name : identifier {};
    struct struct_id : digits {};
    struct struct_byte_size : digits {};
    struct struct_member_name : identifier {};
    struct struct_member_bit_offset : digits {};
    struct struct_member_bit_size : digits {};
    struct struct_member : seq<struct_member_name, one<':'>, type_ref, comma,
                               struct_member_bit_offset, comma, struct_member_bit_size, one<';'>> {
    };
    struct struct_ : seq<struct_name, one<':'>, one<'T'>, struct_id, one<'='>, one<'s'>,
                         struct_byte_size, star<struct_member>, one<';'>> {};

    struct lsym : sor<struct_, array, enum_, type_def, variable> {};

    struct include_file : file_path {};

    using DEFAULT_PARAM_STRING_RULE = until_not_at<dquote>;
    template <typename RULE = DEFAULT_PARAM_STRING_RULE>
    struct param_string : seq<dquote, RULE, dquote> {};

    using DEFAULT_PARAM_TYPE_RULE = until_not_at<comma>;
    template <typename RULE = DEFAULT_PARAM_TYPE_RULE>
    struct param_type : seq<RULE> {};

    using DEFAULT_PARAM_OTHER_RULE = until_not_at<comma>;
    template <typename RULE = DEFAULT_PARAM_OTHER_RULE>
    struct param_other : digits {};

    using DEFAULT_PARAM_DESC_RULE = until_not_at<comma>;
    template <typename RULE = DEFAULT_PARAM_DESC_RULE>
    struct param_desc : seq<RULE> {};

    using DEFAULT_PARAM_VALUE_RULE = until_not_at<eol>;
    template <typename RULE = DEFAULT_PARAM_VALUE_RULE>
    struct param_value : unquoted_string {};

    struct str_stabs : TAO_PEGTL_STRING(".stabs") {};
    struct str_stabd : TAO_PEGTL_STRING(".stabd") {};

    struct stabs_directive_prefix : seq<until<str_stabs>, blanks> {};
    struct stabd_directive_prefix : seq<until<str_stabd>, blanks> {};

    // Match stabs (string) directive
    // Captures: 1:string, 2:type, 3:other, 4:desc, 5:value
    //    204 ;	.stabs	"src/vectrexy.h",132,0,0,Ltext2
    template <typename STRING_RULE, typename TYPE_RULE, typename OTHER_RULE, typename DESC_RULE,
              typename VALUE_RULE>
    struct stabs_directive_for
        : seq<stabs_directive_prefix, param_string<STRING_RULE>, sep, param_type<TYPE_RULE>, sep,
              param_other<OTHER_RULE>, sep, param_desc<DESC_RULE>, sep, param_value<VALUE_RULE>> {};

    // Match stabd (dot) directive
    // Captures: 1:type, 2:other, 3:desc
    //    206;.stabd	68, 0, 61
    template <typename TYPE_RULE, typename OTHER_RULE, typename DESC_RULE>
    struct stabd_directive_for : seq<stabd_directive_prefix, param_type<TYPE_RULE>, sep,
                                     param_other<OTHER_RULE>, sep, param_desc<DESC_RULE>> {};

    // N_LSYM = 128;  // 0x80 Local variable or type definition
    struct stabs_directive_lsym
        : stabs_directive_for<lsym, TAO_PEGTL_STRING("128"), DEFAULT_PARAM_OTHER_RULE,
                              DEFAULT_PARAM_DESC_RULE, DEFAULT_PARAM_VALUE_RULE> {};

    // N_SOL = 132;   // 0x84 Name of include file
    struct stabs_directive_include_file
        : stabs_directive_for<include_file, TAO_PEGTL_STRING("132"), DEFAULT_PARAM_OTHER_RULE,
                              DEFAULT_PARAM_DESC_RULE, DEFAULT_PARAM_VALUE_RULE> {};

    struct stabs_directive : sor<stabs_directive_lsym, stabs_directive_include_file> {};

    // N_SLINE = 68;  // 0x44 Line number in text segment
    struct source_current_line : digits {};
    struct stabd_directive_line
        : stabd_directive_for<TAO_PEGTL_STRING("68"), DEFAULT_PARAM_OTHER_RULE,
                              source_current_line> {};

    struct stabd_directive : sor<stabd_directive_line> {};

    // Match an instruction line
    // Capture: 1:address
    //   072B AE E4         [ 5]  126 	ldx	,s	; tmp33, dest
    struct instr_address : seq<xdigit, xdigit, xdigit, xdigit> {};
    struct instruction
        : seq<blanks, instr_address, until<one<'['>>, any, any, one<']'>, star<any>> {};

    // Match a label line
    // Captures: 1:address, 2:label
    //   086C                     354 Lscope3:
    struct label_address : seq<xdigit, xdigit, xdigit, xdigit> {};
    struct label_name : identifier {};
    struct label : seq<blanks, label_address, blanks, plus<digits>, blanks, label_name, one<':'>> {
    };

    struct grammar : must<seq<sor<instruction, label, stabs_directive, stabd_directive>, eof>> {};

    // Types selected in for parse tree
    template <typename Rule>
    using selector = parse_tree::selector<
        Rule, pegtl::parse_tree::store_content::on<
                  // top-level
                  stabd_directive, stabs_directive,

                  // array
                  array, array_name, array_type_id, array_max_index,
                  // type_def
                  type_def, type_def_name, type_def_id, type_def_range_lower_bound,
                  type_def_range_upper_bound,
                  // variable
                  variable, type_ref, variable_name, type_ref_id, pointer_def, pointer_def_id,
                  pointer_ref_id,
                  // enum
                  enum_, enum_name, enum_id, enum_value_id, enum_value_num,
                  // struct
                  struct_, struct_name, struct_id, struct_byte_size, struct_member_name,
                  struct_member_bit_offset, struct_member_bit_size, struct_member,
                  // instruction
                  instruction, instr_address,
                  // label
                  label, label_address, label_name,
                  // include_file
                  include_file,
                  // line number
                  source_current_line

                  >>;

    using Node = pegtl::parse_tree::node;

    void PrintParseTree(const Node& node, int depth = 0) {
        std::function<void(const Node&, int)> f;
        f = [&f](const Node& node, int depth) {
            for (auto d = depth; d-- > 0;)
                std::cout << " ";
            std::cout << node.type << ": `" << node.string_view() << "`"
                      << "\n";
            for (auto& c : node.children) {
                f(*c, depth + 1);
            }
        };

        assert(node.is_root()); // Root is the only node with no content
        for (auto& c : node.children) {
            f(*c, 0);
        }
        std::cout << "\n";
    }

} // namespace stabs

int main() {
    const std::size_t issues = tao::pegtl::analyze<stabs::grammar>();

    // clang-format off
	std::vector<const char*> source = {
		R"(                            204 ;	.stabs	"src/vectrexy.h",132,0,0,Ltext2)",
		R"(                            206 ;    .stabd	68, 0, 61)",

		//R"(                             31 ;	.stabs	"complex long double:t3=R3;8;0;",128,0,0,0)",
		//R"(                            162 ;	.stabs	"a:7",128,0,0,0)",
		//R"(                             40 ;	.stabs	"int:t7",128,0,0,0)",
		//R"(                             41 ;	.stabs	"char char:t13=r13;0;255;",128,0,0,0)",
		//R"(                             31 ;	.stabs	"complex long double:t3=R3;8;0;",128,0,0,0)",
		//R"(                            162 ;	.stabs	"b:7",128,0,0,0)",
		//R"(                             86 ;	.stabs	"c:25=ar26=r26;0;-1;;0;9;27=ar26;0;10;28=ar26;0;11;7",128,0,0,0)",

		//R"(                            167;.stabs	"a:7",128,0,0,7)",
		//R"(                            168;.stabs	"p:25=*7",128,0,0,5)",
		//R"(                            132;.stabs	"b:30=ar28;0;2;22",128,0,0,18)",
		//R"(                            133;.stabs	"pi:31=ar28;0;3;32=*7",128,0,0,10)",


		//R"(                            169;.stabs	"p2:25",128,0,0,8)",
		//R"(                            170;.stabs	"r3:26=*7",128,0,0,10)",
		//R"(                            171;.stabs	"r4:26",128,0,0,12)",
		//R"(                            172;.stabs	"pp:27=*25",128,0,0,3)",
		//R"(                            173;.stabs	"ppp:28=*27",128,0,0,1)",
		//R"(                            174;.stabs	"rppp:29=*28",128,0,0,14)",

		//// Enum
		//R"(                             55 ;	.stabs	"bool:t22=eFalse:0,True:1,;",128,0,0,0)",
		//R"(                             59 ;	.stabs	"WeekDay:t25=eMonday:0,Tuesday:1,Wednesday:2,EndOfDays:2,Foo:-5000,;",128,0,0,0)",

		//// Struct
		//R"(                             59;.stabs	"Bar:T25=s3x:7,0,8;y:7,8,8;z:7,16,8;;",128,0,0,0)",
		//R"(                             63;.stabs	"Foo:T27=s14a:18,0,32;b:22,32,8;c:25,40,16;bar:26,56,24;d:7,80,6;e:7,86,2;f:7,88,8;p:28=*7,96,16;;",128,0,0,0)",

		//// Instruction
		//R"(   0095 C6 2A         [ 2]   73 	ldb	#42	; D.1687,)"

	 //   // Label
		//R"(   0098                      77 Lscope1:)"

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
