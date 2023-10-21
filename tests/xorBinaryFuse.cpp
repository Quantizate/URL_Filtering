#include "performancecounters/benchmarker.h"
#include <algorithm>
#include <fstream>
#include <iostream>
#include <random>
#include <stdlib.h>
#include <vector>
#include "filterapi.h"

// #define FILTER_SIZE 500000
// #define DATA_SIZE 1000000
// #define TEST_SIZE 500000
// #define BOGUS_SIZE 1000000
// #define QUERY_SIZE (DATA_SIZE - TEST_SIZE)

int data_size = 0, test_size, filter_size, bogus_size = 1000000, query_size;

std::string random_string()
{
  auto randchar = []() -> char
  {
    const char charset[] =
        "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
    const size_t max_index = (sizeof(charset) - 1);
    return charset[rand() % max_index];
  };
  size_t desired_length = rand() % 128;
  std::string str(desired_length, 0);
  std::generate_n(str.begin(), desired_length, randchar);
  return str;
}

int convert(char *str)
{
  int num = 0;

  for (int i = 0; str[i] != '\0'; i++)
    num = num * 10 + str[i] - '0';
  return num;
}

void pretty_print(size_t volume, size_t bytes, // std::string name,
                  event_aggregate agg, FILE *filename)
{
  fprintf(filename, ",%.2f,%.2f,%.2f", bytes / agg.fastest_elapsed_ns(), volume * 1000.0 / agg.fastest_elapsed_ns(), agg.fastest_elapsed_ns() / volume);
}

// data size, test size, true positive, true negative, false positive, false negative, bogus size, false_pos_bogus, duplicates data
void writeOutput(size_t tp, size_t tn, size_t fp, size_t fn, size_t fp_bogus, int dup_num, FILE *filename)
{
  fprintf(filename, "%d,%d,%zu,%zu,%zu,%zu,%d,%zu,%d\n", data_size, test_size, tp, tn, fp, fn, bogus_size, fp_bogus, dup_num);
}

// data_size, test_size, total_data_volume, average_len (bytes/name), test_volume, filter_volume, %usage(wrt to test_volume), usuage (wrt to test_size[bits / entry])
void writeStat1(size_t input_volume, size_t test_volume, size_t filter_volume, FILE *filename)
{
  fprintf(filename, "%d,%d,%zu,%.1f,%zu,%zu,%.2f %%,%.1f\n", data_size, test_size, input_volume, double(input_volume) / data_size, test_volume, filter_volume, 100.0 * filter_volume / test_volume, 8.0 * filter_volume / test_size);
}

// data_size, test_size, Benchmarking queries[time/entry, GB/s, Ma/s, ns/d], Benchmarking construction[time/entry, GB/s, Ma/s, ns/d]

void writeStat2(FILE *filename)
{
  fprintf(filename, "%d,%d", data_size, test_size);
}

uint64_t simple_hash(const std::string &line)
{
  uint64_t h = 0;
  for (unsigned char c : line)
  {
    h = (h * 177) + c;
  }
  h ^= line.size();
  return h;
}

int main(int argc, char **argv)
{
  std::vector<std::string> inputs;

  size_t volume = 0;

  if (argc == 1)
  {
    printf("You must pass a list of URLs (one per line). For instance:\n");
    printf("./benchmark data/top-1m.csv  \n");
    return EXIT_FAILURE;
  }
  else
  {
    std::ifstream input(argv[1]);
    if (!input)
    {
      std::cerr << "Could not open " << argv[1] << std::endl;
      exit(EXIT_FAILURE);
    }
    for (std::string line; std::getline(input, line);)
    {
      std::string ref = line;
      ref.erase(std::find_if(ref.rbegin(), ref.rend(),
                             [](unsigned char ch)
                             { return !std::isspace(ch); })
                    .base(),
                ref.end());
      volume += ref.size();
      inputs.push_back(ref);
      data_size++;
    }
    // std::cout << "loaded " << inputs.size() << " names" << std::endl;
    // std::cout << "average length " << double(volume) / inputs.size()
    //           << " bytes/name" << std::endl;
  }
  // printf("\n");
  FILE *data_reliability = fopen(argv[3], "a");
  FILE *stat1 = fopen(argv[4], "a");
  FILE *stat2 = fopen(argv[5], "a");

  test_size = convert(argv[2]);
  query_size = data_size - test_size;

  bool is_ok;
  size_t fp_bogus = 0;
  size_t filter_volume = 0;
  size_t falsePositive = 0, falseNegative = 0, truePositive = 0, trueNegative = 0;

  std::vector<std::pair<bool, bool>> dataValidity(data_size, {false, false}); // original, modified

  /* We are going to check for duplicates. If you have too many duplicates, something might be wrong. */
  int dup_num = 0;
  std::sort(inputs.begin(), inputs.end());
  auto dup_str = std::adjacent_find(inputs.begin(), inputs.end());
  while (dup_str != inputs.end())
  {
    // std::cout << "duplicated string " << *dup_str << std::endl;
    dup_str = std::adjacent_find(dup_str + 1, inputs.end());
    dup_num++;
  }
  size_t bytes = 0;
  for (int i = 0; i < test_size; i++)
  {
    bytes += inputs[i].size();
  }

  std::vector<uint64_t> test_hashes(test_size), hashes(data_size), bogus_hashes(bogus_size);
  for (size_t i = 0; i < (size_t)data_size; i++)
  {
    hashes[i] = simple_hash(inputs[i]);
    if (i < (size_t)test_size)
    {
      test_hashes[i] = hashes[i];
      dataValidity[i].first = true;
    }
  }

  size_t size = test_hashes.size();

  std::vector<std::string> query_set_bogus;

  for (size_t i = 0; i < bogus_size; i++)
  {
    query_set_bogus.push_back(random_string());
  }

  for (int i = 0; i < bogus_size; i++)
  {
    bogus_hashes[i] = simple_hash(query_set_bogus[i]);
  }

  // printf("\n");
  // printf("Test size(added to filter): %d \n", test_size);
  // printf("Query size(not added to filter): %d \n", query_size);
  // printf("Bogus size(randomly generated strings): %d \n", bogus_size);
  // printf("\n");

  // printf("-------------- Xor Binary Fuse - 8 Naive Filter --------------\n");
  // // Memory allocation (trivial):
  // xorbinaryfusefilter_naive::XorBinaryFuseFilter<uint64_t, uint8_t> xbf_8_naive(filter_size), xbf_8_test(test_size);

  // // Construction
  // xbf_8_naive.AddAll(test_hashes, 0, test_size);

  // // Let us check the size of the filter in bytes:
  // filter_volume = xbf_8_naive.SizeInBytes();
  // // printf("\nfilter memory usage : %zu bytes (%.1f %% of input)\n", filter_volume,
  // //        100.0 * filter_volume / bytes);
  // // printf("\nfilter memory usage : %1.f bits/entry\n",
  // //        8.0 * filter_volume / test_hashes.size());
  // // printf("\n");

  // // Let us test the query with bogus strings

  // fp_bogus = 0;
  // for (int i = 0; i < bogus_size; i++)
  // {
  //   bool in_set = xbf_8_naive.Contain(bogus_hashes[i]);
  //   if (in_set)
  //   {
  //     fp_bogus++;
  //   }
  // }

  // // printf("Bogus false-positives: %zu\n", fpp);
  // // printf("Bogus false-positive rate %f\n", fpp / double(query_set_bogus.size()));

  // // volatile size_t basic_count = 0;
  // // printf("Benchmarking queries:\n");

  // // pretty_print(query_set_bogus.size(), bogus_volume, "binary_fuse16_contain",
  // //              bench([&query_set_bogus, &filter, &basic_count]()
  // //                    {
  // //                for (std::string &ref : query_set_bogus) {
  // //                  basic_count +=
  // //                      filter.Contain(simple_hash(ref));
  // //                } }));
  // // printf("Benchmarking construction speed\n");

  // // pretty_print(inputs.size(), bytes, "binary_fuse16_populate",
  // //              bench([&test_hashes, &filter4, &size]()
  // //                    {
  // //               for(uint64_t &ref : test_hashes){
  // //                 size += 64;
  // //                filter4.Add(ref);} }));

  // writeStat2(stat2);

  // // Benchmarking queries:
  // pretty_print(inputs.size(), bytes,
  //              bench([&hashes, &xbf_8_naive, &dataValidity]()
  //                    {
  //                for (int i = 0; i < data_size; i++)
  //                {
  //                  dataValidity[i].second = xbf_8_naive.Contain(hashes[i]);
  //                } }),
  //              stat2);

  // // Benchmarking construction speed
  // pretty_print(test_hashes.size(), bytes,
  //              bench([&test_hashes, &xbf_8_test, &size]()
  //                    { xbf_8_test.AddAll(test_hashes.data(), 0, size); }),
  //              stat2);

  // fprintf(stat2, "\n");

  // writeStat1(volume, bytes, filter_volume, stat1);

  // // Testing
  // for (int i = 0; i < data_size; i++)
  // {
  //   dataValidity[i].second = xbf_8_naive.Contain(hashes[i]);
  // }

  // falsePositive = 0;
  // falseNegative = 0;
  // truePositive = 0;
  // trueNegative = 0;

  // for (int i = 0; i < data_size; i++)
  // {
  //   if (dataValidity[i].first == true && dataValidity[i].second == true)
  //     truePositive++;
  //   else if (dataValidity[i].first == true && dataValidity[i].second == false)
  //     falseNegative++;
  //   else if (dataValidity[i].first == false && dataValidity[i].second == true)
  //     falsePositive++;
  //   else if (dataValidity[i].first == false && dataValidity[i].second == false)
  //     trueNegative++;
  // }

  // // printf("\n");
  // // printf("Tested with total data set (test + query): %d \n", data_size);
  // // printf("True Positive: %zu", truePositive);
  // // printf("\t");
  // // printf("True Negative: %zu", trueNegative);
  // // printf("\n");
  // // printf("False Positive: %zu", falsePositive);
  // // printf("\t");
  // // printf("False Negative: %zu", falseNegative);
  // // printf("\n");
  // // printf("\n");

  // writeOutput(truePositive, trueNegative, falsePositive, falseNegative, fp_bogus, dup_num, data_reliability);

  // printf("-------------- Xor Binary Fuse - 16 Naive Filter --------------\n");
  // xorbinaryfusefilter_naive::XorBinaryFuseFilter<uint64_t, uint16_t> xbf_16_naive(filter_size), xbf_16_test(test_size);

  // // Construction
  // xbf_16_naive.AddAll(test_hashes, 0, test_size);

  // // Let us check the size of the filter in bytes:
  // filter_volume = xbf_16_naive.SizeInBytes();
  // // printf("\nfilter memory usage : %zu bytes (%.1f %% of input)\n", filter_volume,
  // //        100.0 * filter_volume / bytes);
  // // printf("\nfilter memory usage : %1.f bits/entry\n",
  // //        8.0 * filter_volume / test_hashes.size());
  // // printf("\n");

  // // Let us test the query with bogus strings

  // fp_bogus = 0;
  // for (int i = 0; i < bogus_size; i++)
  // {
  //   bool in_set = xbf_16_naive.Contain(bogus_hashes[i]);
  //   if (in_set)
  //   {
  //     fp_bogus++;
  //   }
  // }

  // // printf("Bogus false-positives: %zu\n", fpp);
  // // printf("Bogus false-positive rate %f\n", fpp / double(query_set_bogus.size()));

  // // volatile size_t basic_count = 0;
  // // printf("Benchmarking queries:\n");

  // // pretty_print(query_set_bogus.size(), bogus_volume, "binary_fuse16_contain",
  // //              bench([&query_set_bogus, &filter, &basic_count]()
  // //                    {
  // //                for (std::string &ref : query_set_bogus) {
  // //                  basic_count +=
  // //                      filter.Contain(simple_hash(ref));
  // //                } }));
  // // printf("Benchmarking construction speed\n");

  // // pretty_print(inputs.size(), bytes, "binary_fuse16_populate",
  // //              bench([&test_hashes, &filter4, &size]()
  // //                    {
  // //               for(uint64_t &ref : test_hashes){
  // //                 size += 64;
  // //                filter4.Add(ref);} }));

  // writeStat2(stat2);

  // // Benchmarking queries:
  // pretty_print(inputs.size(), bytes,
  //              bench([&hashes, &xbf_16_naive, &dataValidity]()
  //                    {
  //                for (int i = 0; i < data_size; i++)
  //                {
  //                  dataValidity[i].second = xbf_16_naive.Contain(hashes[i]);
  //                } }),
  //              stat2);

  // // Benchmarking construction speed
  // pretty_print(test_hashes.size(), bytes,
  //              bench([&test_hashes, &xbf_16_test, &size]()
  //                    { xbf_16_test.AddAll(test_hashes.data(), 0, size); }),
  //              stat2);

  // fprintf(stat2, "\n");

  // writeStat1(volume, bytes, filter_volume, stat1);

  // // Testing
  // for (int i = 0; i < data_size; i++)
  // {
  //   dataValidity[i].second = xbf_16_naive.Contain(hashes[i]);
  // }

  // falsePositive = 0;
  // falseNegative = 0;
  // truePositive = 0;
  // trueNegative = 0;

  // for (int i = 0; i < data_size; i++)
  // {
  //   if (dataValidity[i].first == true && dataValidity[i].second == true)
  //     truePositive++;
  //   else if (dataValidity[i].first == true && dataValidity[i].second == false)
  //     falseNegative++;
  //   else if (dataValidity[i].first == false && dataValidity[i].second == true)
  //     falsePositive++;
  //   else if (dataValidity[i].first == false && dataValidity[i].second == false)
  //     trueNegative++;
  // }

  // // printf("\n");
  // // printf("Tested with total data set (test + query): %d \n", data_size);
  // // printf("True Positive: %zu", truePositive);
  // // printf("\t");
  // // printf("True Negative: %zu", trueNegative);
  // // printf("\n");
  // // printf("False Positive: %zu", falsePositive);
  // // printf("\t");
  // // printf("False Negative: %zu", falseNegative);
  // // printf("\n");
  // // printf("\n");

  // writeOutput(truePositive, trueNegative, falsePositive, falseNegative, fp_bogus, dup_num, data_reliability);

  // printf("-------------- Xor Binary Fuse - 8 Filter --------------\n");
  // xorbinaryfusefilter_lowmem::XorBinaryFuseFilter<uint64_t, uint8_t> xbf_8(filter_size);

  // xbf_8.AddAll(test_hashes, 0, test_size);

  // // Let us check the size of the filter in bytes:
  // ssize_t filter_volume = xbf_8.SizeInBytes();
  // printf("\nfilter memory usage : %zu bytes (%.1f %% of input)\n", filter_volume,
  //        100.0 * filter_volume / bytes);
  // printf("\nfilter memory usage : %1.f bits/entry\n",
  //        8.0 * filter_volume / test_hashes.size());
  // printf("\n");

  // // Let us test the query with bogus strings
  // std::vector<std::string> query_set_bogus;
  // size_t bogus_volume = 0;
  // for (size_t i = 0; i < bogus_size; i++)
  //   query_set_bogus.push_back(random_string());

  // size_t fpp = 0;
  // for (int i = 0; i < bogus_size; i++)
  // {
  //   bogus_volume += query_set_bogus[i].size();
  //   bogus_hashes[i] = simple_hash(query_set_bogus[i]);
  //   bool in_set = xbf_8.Contain(bogus_hashes[i]);
  //   if (in_set)
  //   {
  //     fpp++;
  //   }
  // }

  // printf("Bogus false-positives: %zu\n", fpp);
  // printf("Bogus false-positive rate %f\n", fpp / double(query_set_bogus.size()));

  // // Testing
  // for (int i = 0; i < data_size; i++)
  // {
  //   dataValidity[i].second = xbf_8.Contain(hashes[i]);
  // }

  // ssize_t falseNegative, falsePositive, trueNegative, truePositive;
  // falsePositive = falseNegative = truePositive = trueNegative = 0;

  // for (int i = 0; i < data_size; i++)
  // {
  //   if (dataValidity[i].first == true && dataValidity[i].second == true)
  //     truePositive++;
  //   else if (dataValidity[i].first == true && dataValidity[i].second == false)
  //     falseNegative++;
  //   else if (dataValidity[i].first == false && dataValidity[i].second == true)
  //     falsePositive++;
  //   else if (dataValidity[i].first == false && dataValidity[i].second == false)
  //     trueNegative++;
  // }

  // printf("\n");
  // printf("Tested with total data set (test + query): %d \n", data_size);
  // printf("True Positive: %zu", truePositive);
  // printf("\t");
  // printf("True Negative: %zu", trueNegative);
  // printf("\n");
  // printf("False Positive: %zu", falsePositive);
  // printf("\t");
  // printf("False Negative: %zu", falseNegative);
  // printf("\n");
  // printf("\n");

  // writeOutput("Xor Binary Fuse - 8", truePositive, trueNegative, falsePositive, falseNegative, outputFile, fpp);

  // printf("-------------- Xor Binary Fuse - 16 Filter --------------\n");
  // xorbinaryfusefilter_lowmem::XorBinaryFuseFilter<uint64_t, uint16_t> xbf_16(filter_size), xbf_16_test(test_size);

  // // Construction
  // xbf_16.AddAll(test_hashes, 0, test_size);

  // // Let us check the size of the filter in bytes:
  // filter_volume = xbf_16.SizeInBytes();
  // // printf("\nfilter memory usage : %zu bytes (%.1f %% of input)\n", filter_volume,
  // //        100.0 * filter_volume / bytes);
  // // printf("\nfilter memory usage : %1.f bits/entry\n",
  // //        8.0 * filter_volume / test_hashes.size());
  // // printf("\n");

  // // Let us test the query with bogus strings

  // fp_bogus = 0;
  // for (int i = 0; i < bogus_size; i++)
  // {
  //   bool in_set = xbf_16.Contain(bogus_hashes[i]);
  //   if (in_set)
  //   {
  //     fp_bogus++;
  //   }
  // }

  // writeStat2(stat2);

  // // Benchmarking queries:
  // pretty_print(inputs.size(), bytes,
  //              bench([&hashes, &xbf_16, &dataValidity]()
  //                    {
  //                for (int i = 0; i < data_size; i++)
  //                {
  //                  dataValidity[i].second = xbf_16.Contain(hashes[i]);
  //                } }),
  //              stat2);

  // // Benchmarking construction speed
  // pretty_print(test_hashes.size(), bytes,
  //              bench([&test_hashes, &xbf_16_test, &size]()
  //                    { xbf_16_test.AddAll(test_hashes.data(), 0, size); }),
  //              stat2);

  // fprintf(stat2, "\n");

  // writeStat1(volume, bytes, filter_volume, stat1);

  // // Testing
  // for (int i = 0; i < data_size; i++)
  // {
  //   dataValidity[i].second = xbf_16.Contain(hashes[i]);
  // }

  // falsePositive = 0;
  // falseNegative = 0;
  // truePositive = 0;
  // trueNegative = 0;

  // for (int i = 0; i < data_size; i++)
  // {
  //   if (dataValidity[i].first == true && dataValidity[i].second == true)
  //     truePositive++;
  //   else if (dataValidity[i].first == true && dataValidity[i].second == false)
  //     falseNegative++;
  //   else if (dataValidity[i].first == false && dataValidity[i].second == true)
  //     falsePositive++;
  //   else if (dataValidity[i].first == false && dataValidity[i].second == false)
  //     trueNegative++;
  // }

  // // printf("\n");
  // // printf("Tested with total data set (test + query): %d \n", data_size);
  // // printf("True Positive: %zu", truePositive);
  // // printf("\t");
  // // printf("True Negative: %zu", trueNegative);
  // // printf("\n");
  // // printf("False Positive: %zu", falsePositive);
  // // printf("\t");
  // // printf("False Negative: %zu", falseNegative);
  // // printf("\n");
  // // printf("\n");

  // writeOutput(truePositive, trueNegative, falsePositive, falseNegative, fp_bogus, dup_num, data_reliability);

  // printf("-------------- Xor Binary Fuse - 8 4-wise Filter --------------\n");
  // xorbinaryfusefilter_lowmem4wise::XorBinaryFuseFilter<uint64_t, uint8_t> xbf_8_4(filter_size);

  // xbf_8_4.AddAll(test_hashes, 0, test_size);

  // // Let us check the size of the filter in bytes:
  // filter_volume = xbf_8_4.SizeInBytes();
  // printf("\nfilter memory usage : %zu bytes (%.1f %% of input)\n", filter_volume,
  //        100.0 * filter_volume / bytes);
  // printf("\nfilter memory usage : %1.f bits/entry\n",
  //        8.0 * filter_volume / test_hashes.size());
  // printf("\n");
  // // Let us test the query with bogus strings

  // fpp = 0;
  // for (auto &ref : bogus_hashes)
  // {
  //   bool in_set = xbf_8_4.Contain(ref);
  //   if (in_set)
  //   {
  //     fpp++;
  //   }
  // }

  // printf("Bogus false-positives: %zu\n", fpp);
  // printf("Bogus false-positive rate %f\n", fpp / double(query_set_bogus.size()));

  // // Testing
  // for (int i = 0; i < data_size; i++)
  // {
  //   dataValidity[i].second = xbf_8_4.Contain(hashes[i]);
  // }

  // falsePositive = falseNegative = truePositive = trueNegative = 0;

  // for (int i = 0; i < data_size; i++)
  // {
  //   if (dataValidity[i].first == true && dataValidity[i].second == true)
  //     truePositive++;
  //   else if (dataValidity[i].first == true && dataValidity[i].second == false)
  //     falseNegative++;
  //   else if (dataValidity[i].first == false && dataValidity[i].second == true)
  //     falsePositive++;
  //   else if (dataValidity[i].first == false && dataValidity[i].second == false)
  //     trueNegative++;
  // }

  // printf("\n");
  // printf("Tested with total data set (test + query): %d \n", data_size);
  // printf("True Positive: %zu", truePositive);
  // printf("\t");
  // printf("True Negative: %zu", trueNegative);
  // printf("\n");
  // printf("False Positive: %zu", falsePositive);
  // printf("\t");
  // printf("False Negative: %zu", falseNegative);
  // printf("\n");
  // printf("\n");

  // writeOutput("Xor Binary Fuse - 8 4-wise", truePositive, trueNegative, falsePositive, falseNegative, outputFile, fpp);

  // printf("-------------- Xor Binary Fuse - 16 4-wise Filter --------------\n");
  xorbinaryfusefilter_lowmem4wise::XorBinaryFuseFilter<uint64_t, uint16_t> xbf_16_4(filter_size), xbf_16_4_test(test_size);

  // Construction
  xbf_16_4.AddAll(test_hashes, 0, test_size);

  // Let us check the size of the filter in bytes:
  filter_volume = xbf_16_4.SizeInBytes();
  // printf("\nfilter memory usage : %zu bytes (%.1f %% of input)\n", filter_volume,
  //        100.0 * filter_volume / bytes);
  // printf("\nfilter memory usage : %1.f bits/entry\n",
  //        8.0 * filter_volume / test_hashes.size());
  // printf("\n");

  // Let us test the query with bogus strings

  fp_bogus = 0;
  for (int i = 0; i < bogus_size; i++)
  {
    bool in_set = xbf_16_4.Contain(bogus_hashes[i]);
    if (in_set)
    {
      fp_bogus++;
    }
  }

  writeStat2(stat2);

  // Benchmarking queries:
  pretty_print(inputs.size(), bytes,
               bench([&hashes, &xbf_16_4, &dataValidity]()
                     {
                 for (int i = 0; i < data_size; i++)
                 {
                   dataValidity[i].second = xbf_16_4.Contain(hashes[i]);
                 } }),
               stat2);

  // Benchmarking construction speed
  pretty_print(test_hashes.size(), bytes,
               bench([&test_hashes, &xbf_16_4_test, &size]()
                     { xbf_16_4_test.AddAll(test_hashes.data(), 0, size); }),
               stat2);

  fprintf(stat2, "\n");

  writeStat1(volume, bytes, filter_volume, stat1);

  // Testing
  for (int i = 0; i < data_size; i++)
  {
    dataValidity[i].second = xbf_16_4.Contain(hashes[i]);
  }

  falsePositive = 0;
  falseNegative = 0;
  truePositive = 0;
  trueNegative = 0;

  for (int i = 0; i < data_size; i++)
  {
    if (dataValidity[i].first == true && dataValidity[i].second == true)
      truePositive++;
    else if (dataValidity[i].first == true && dataValidity[i].second == false)
      falseNegative++;
    else if (dataValidity[i].first == false && dataValidity[i].second == true)
      falsePositive++;
    else if (dataValidity[i].first == false && dataValidity[i].second == false)
      trueNegative++;
  }

  writeOutput(truePositive, trueNegative, falsePositive, falseNegative, fp_bogus, dup_num, data_reliability);

  fclose(data_reliability);
  fclose(stat1);
  fclose(stat2);

  return EXIT_SUCCESS;
}