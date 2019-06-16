solution "lucy"
	flags { "Cpp17" }
	configurations { "Debug", "RelWithDebInfo" }
	includedirs {"../src" }
	language "C++"
	location "tmp"
	targetdir "tmp/bin"
	platforms { "x64" }
	flags { "Symbols" }
	
	project "lucy"
		kind "ConsoleApp"
		files { "../src/**.h", "../src/**.cpp" }