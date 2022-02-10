# Generated by devtools/yamaker from nixpkgs 5852a21819542e6809f68ba5a798600e69874e76.

LIBRARY() 

OWNER(g:cpp-contrib)
 
VERSION(1.2.11)

ORIGINAL_SOURCE(https://www.zlib.net/fossils/zlib-1.2.11.tar.gz)

OPENSOURCE_EXPORT_REPLACEMENT(
    CMAKE ZLIB
    CMAKE_TARGET ZLIB::ZLIB
    CONAN zlib/1.2.11
)
LICENSE(Zlib)

LICENSE_TEXTS(.yandex_meta/licenses.list.txt)

ADDINCL(
    GLOBAL contrib/libs/zlib/include
)

NO_COMPILER_WARNINGS()
 
NO_RUNTIME()

SRCS( 
    adler32.c 
    compress.c 
    crc32.c 
    deflate.c 
    gzclose.c 
    gzlib.c 
    gzread.c 
    gzwrite.c 
    infback.c 
    inffast.c 
    inflate.c 
    inftrees.c 
    trees.c 
    uncompr.c 
    zutil.c 
) 
 
END() 
