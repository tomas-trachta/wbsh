/**
 * @file make_defaults.cpp
 * @brief The variables make defines before reading a makefile, and GNU
 *        make's built-in variables and suffix rules (`-R` / `-r` turn them
 *        off), read as a makefile of their own.
 */

#include "make_internal.h"

#include <string>

#include "executor.h"

namespace wbsh::make_detail {

	static const char* const kVersion = "4.4.1";

	static const char* const kFeatures = "target-specific order-only second-expansion else-if "
		"shortest-stem undefine oneshell nocomment grouped-target extra-prereqs notintermediate "
		"shell-export";

	static const char* const kBuiltinVariables =
		"AR = ar\n"
		"ARFLAGS = rv\n"
		"AS = as\n"
		"CC = cc\n"
		"CXX = g++\n"
		"CPP = $(CC) -E\n"
		"FC = f77\n"
		"F77 = $(FC)\n"
		"LD = ld\n"
		"LEX = lex\n"
		"YACC = yacc\n"
		"LINT = lint\n"
		"M2C = m2c\n"
		"PC = pc\n"
		"CO = co\n"
		"GET = get\n"
		"MAKEINFO = makeinfo\n"
		"TEX = tex\n"
		"TEXI2DVI = texi2dvi\n"
		"WEAVE = weave\n"
		"CWEAVE = cweave\n"
		"TANGLE = tangle\n"
		"CTANGLE = ctangle\n"
		"RM = rm -f\n"
		"OUTPUT_OPTION = -o $@\n"
		"COMPILE.c = $(CC) $(CFLAGS) $(CPPFLAGS) $(TARGET_ARCH) -c\n"
		"LINK.c = $(CC) $(CFLAGS) $(CPPFLAGS) $(LDFLAGS) $(TARGET_ARCH)\n"
		"COMPILE.cc = $(CXX) $(CXXFLAGS) $(CPPFLAGS) $(TARGET_ARCH) -c\n"
		"LINK.cc = $(CXX) $(CXXFLAGS) $(CPPFLAGS) $(LDFLAGS) $(TARGET_ARCH)\n"
		"COMPILE.C = $(COMPILE.cc)\n"
		"COMPILE.cpp = $(COMPILE.cc)\n"
		"LINK.C = $(LINK.cc)\n"
		"LINK.cpp = $(LINK.cc)\n"
		"LINK.o = $(CC) $(LDFLAGS) $(TARGET_ARCH)\n"
		"COMPILE.f = $(FC) $(FFLAGS) $(TARGET_ARCH) -c\n"
		"LINK.f = $(FC) $(FFLAGS) $(LDFLAGS) $(TARGET_ARCH)\n"
		"COMPILE.s = $(AS) $(ASFLAGS) $(TARGET_MACH)\n"
		"LINK.s = $(CC) $(ASFLAGS) $(LDFLAGS) $(TARGET_MACH)\n"
		"COMPILE.S = $(CC) $(ASFLAGS) $(CPPFLAGS) $(TARGET_MACH) -c\n"
		"LINK.S = $(CC) $(ASFLAGS) $(CPPFLAGS) $(LDFLAGS) $(TARGET_MACH)\n"
		"PREPROCESS.S = $(CC) -E $(CPPFLAGS)\n"
		"YACC.y = $(YACC) $(YFLAGS)\n"
		"LEX.l = $(LEX) $(LFLAGS) -t\n";

	static const char* const kBuiltinRules =
		".SUFFIXES: .out .a .ln .o .c .cc .C .cpp .p .f .F .m .r .y .l .ym .yl .s .S .mod .sym"
		" .def .h .info .dvi .tex .texinfo .texi .txinfo .w .ch .web .sh .elc .el\n"
		".o:\n\t$(LINK.o) $^ $(LOADLIBES) $(LDLIBS) -o $@\n"
		".c:\n\t$(LINK.c) $^ $(LOADLIBES) $(LDLIBS) -o $@\n"
		".cc:\n\t$(LINK.cc) $^ $(LOADLIBES) $(LDLIBS) -o $@\n"
		".C:\n\t$(LINK.C) $^ $(LOADLIBES) $(LDLIBS) -o $@\n"
		".cpp:\n\t$(LINK.cpp) $^ $(LOADLIBES) $(LDLIBS) -o $@\n"
		".f:\n\t$(LINK.f) $^ $(LOADLIBES) $(LDLIBS) -o $@\n"
		".s:\n\t$(LINK.s) $^ $(LOADLIBES) $(LDLIBS) -o $@\n"
		".S:\n\t$(LINK.S) $^ $(LOADLIBES) $(LDLIBS) -o $@\n"
		".sh:\n\tcat $< >$@\n\tchmod a+x $@\n"
		".c.o:\n\t$(COMPILE.c) $(OUTPUT_OPTION) $<\n"
		".cc.o:\n\t$(COMPILE.cc) $(OUTPUT_OPTION) $<\n"
		".C.o:\n\t$(COMPILE.C) $(OUTPUT_OPTION) $<\n"
		".cpp.o:\n\t$(COMPILE.cpp) $(OUTPUT_OPTION) $<\n"
		".f.o:\n\t$(COMPILE.f) $(OUTPUT_OPTION) $<\n"
		".s.o:\n\t$(COMPILE.s) -o $@ $<\n"
		".S.o:\n\t$(COMPILE.S) -o $@ $<\n"
		".S.s:\n\t$(PREPROCESS.S) $< > $@\n"
		".y.c:\n\t$(YACC.y) $<\n\tmv -f y.tab.c $@\n"
		".l.c:\n\t@$(RM) $@\n\t$(LEX.l) $< > $@\n";

	static void setDefault(Make& make, const std::string& name, const std::string& value) {
		setGlobal(make, name, value, Origin::Default);
	}

	static void readBuiltinText(Make& make, const char* text) {
		make.reading_builtins = true;
		readMakefileText(make, "<builtin>", text);
		make.reading_builtins = false;
	}

	void defineDefaultVariables(Make& make) {
		setDefault(make, "MAKE_COMMAND", "make");
		setDefault(make, "MAKE", "make");
		setDefault(make, "MAKE_VERSION", kVersion);
		setDefault(make, "MAKE_HOST", "x86_64-w64-mingw32");
		setDefault(make, ".FEATURES", kFeatures);
		setDefault(make, ".SHELLFLAGS", "-c");
		setGlobal(make, "SHELL", make.exec.pathConv().toPosix(make.self_path), Origin::File);
		setGlobal(make, ".DEFAULT_GOAL", std::string(), Origin::File);
		setGlobal(make, "MAKEFILE_LIST", std::string(), Origin::File);
		if (!make.options.no_builtin_variables) readBuiltinText(make, kBuiltinVariables);
	}

	void readBuiltinRules(Make& make) {
		if (make.options.no_builtin_rules) return;

		readBuiltinText(make, kBuiltinRules);
		setDefault(make, "SUFFIXES", joinWords(make.suffixes));
	}

}  // namespace wbsh::make_detail
