// ucache-tree-dump: emit the parsed file/tree structure as JSON in the same
// shape as the uproot reference dumper (the ground truth); the
// differential comparison of the two is the parser's gate.
#include "TreeMeta.h"

#include <cstdio>

using namespace ucache::transpose;

namespace {
void jstr(const std::string& s) {
  std::putchar('"');
  for (char ch : s) {
    if (ch == '"' || ch == '\\')
      std::putchar('\\');
    std::putchar(ch);
  }
  std::putchar('"');
}
} // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: %s FILE.root [tree]\n", argv[0]);
    return 2;
  }
  FileMeta fm = parseFile(argv[1], argc > 2 ? argv[2] : "Events");
  if (!fm.error.empty()) {
    std::fprintf(stderr, "parse failed: %s\n", fm.error.c_str());
    return 1;
  }
  std::printf("{\"file\":{\"large\":%s,\"header_seek_width\":%d,\"dir_seek_width\":%d,"
              "\"fend\":%lld,\"keyslist_seek\":%lld},",
              fm.large ? "true" : "false", fm.headerSeekWidth, fm.dirSeekWidth,
              static_cast<long long>(fm.fend), static_cast<long long>(fm.keyslistSeek));
  std::printf("\"tree_key\":{\"seek\":%lld,\"nbytes\":%d,\"objlen\":%d,\"keylen\":%d},",
              static_cast<long long>(fm.treeKey.seekkey), fm.treeKey.nbytes,
              fm.treeKey.objlen, fm.treeKey.keylen);
  std::printf("\"tree\":{\"name\":");
  jstr(argc > 2 ? argv[2] : "Events");
  std::printf(",\"n_branches\":%zu,\"branches\":[", fm.branches.size());
  bool first = true;
  for (const auto& b : fm.branches) {
    if (!first)
      std::putchar(',');
    first = false;
    std::printf("{\"name\":");
    jstr(b.name);
    std::printf(",\"write_basket\":%d,\"max_baskets\":%u,\"basket_seek\":[",
                b.writeBasket, b.maxBaskets);
    for (size_t i = 0; i < b.basketSeek.size(); ++i)
      std::printf(i ? ",%lld" : "%lld", static_cast<long long>(b.basketSeek[i]));
    std::printf("],\"basket_bytes\":[");
    for (size_t i = 0; i < b.basketBytes.size(); ++i)
      std::printf(i ? ",%d" : "%d", b.basketBytes[i]);
    std::printf("],\"leaf_class\":");
    jstr(b.leafClass);
    std::printf(",\"leaf_count\":");
    jstr(b.leafCount);
    std::putchar('}');
  }
  std::printf("]}}\n");
  return 0;
}
