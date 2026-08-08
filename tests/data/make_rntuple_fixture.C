// Build a tiny RNTuple that still exercises what the recompressor cares about:
// bit-packed booleans, several clusters, identical (shared) pages, and a
// variable-length column.
#include <ROOT/RNTupleModel.hxx>
#include <ROOT/RNTupleWriter.hxx>
#include <ROOT/RNTupleWriteOptions.hxx>
#include <cstdint>
#include <vector>

void mkfixture(const char* path, int nentries = 2000, int clusterBytes = 4096,
               int compression = 505) {
  auto model = ROOT::RNTupleModel::Create();
  auto fi = model->MakeField<std::int32_t>("i");       // varying
  auto fb = model->MakeField<bool>("flag");            // BIT-PACKED, 1 bit
  auto fk = model->MakeField<std::int32_t>("konst");   // constant -> identical pages
  auto fx = model->MakeField<float>("x");
  auto fv = model->MakeField<std::vector<float>>("v"); // index + payload columns

  ROOT::RNTupleWriteOptions opts;
  opts.SetCompression(compression);
  opts.SetApproxZippedClusterSize(clusterBytes);       // force several clusters

  auto w = ROOT::RNTupleWriter::Recreate(std::move(model), "Events", path, opts);
  for (int n = 0; n < nentries; ++n) {
    *fi = n;
    *fb = (n % 3 == 0);
    *fk = 42;
    *fx = n * 0.5f;
    fv->clear();
    for (int k = 0; k < n % 4; ++k) fv->push_back(k + 0.25f);
    w->Fill();
  }
}
