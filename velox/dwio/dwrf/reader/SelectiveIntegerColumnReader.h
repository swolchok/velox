/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

#include "velox/dwio/dwrf/reader/SelectiveColumnReaderInternal.h"

namespace facebook::velox::dwrf {

// Abstract class for format and encoding-independent parts of reading ingeger
// columns.
class SelectiveIntegerColumnReader : public SelectiveColumnReader {
 public:
  SelectiveIntegerColumnReader(
      std::shared_ptr<const dwio::common::TypeWithId> requestedType,
      StripeStreams& stripe,
      common::ScanSpec* scanSpec,
      const TypePtr& type)
      : SelectiveColumnReader(
            std::move(requestedType),
            stripe,
            scanSpec,
            type) {}

  void getValues(RowSet rows, VectorPtr* result) override {
    getIntValues(rows, nodeType_->type.get(), result);
  }

 protected:
  // Switches based on filter type between different readHelper instantiations.
  template <typename Reader, bool isDense, typename ExtractValues>
  void processFilter(
      common::Filter* filter,
      ExtractValues extractValues,
      RowSet rows);

  // Switches based on the type of ValueHook between different readWithVisitor
  // instantiations.
  template <typename Reader, bool isDence>
  void processValueHook(RowSet rows, ValueHook* hook);

  // Instantiates a Visitor based on type, isDense, value processing.
  template <
      typename Reader,
      typename TFilter,
      bool isDense,
      typename ExtractValues>
  void
  readHelper(common::Filter* filter, RowSet rows, ExtractValues extractValues);

  // The common part of integer reading. calls the appropriate
  // instantiation of processValueHook or processFilter based on
  // possible value hook, filter and denseness.
  template <typename Reader>
  void readCommon(RowSet rows);
};

template <
    typename Reader,
    typename TFilter,
    bool isDense,
    typename ExtractValues>
void SelectiveIntegerColumnReader::readHelper(
    common::Filter* filter,
    RowSet rows,
    ExtractValues extractValues) {
  switch (valueSize_) {
    case 2:
      reinterpret_cast<Reader*>(this)->Reader::readWithVisitor(
          rows,
          ColumnVisitor<int16_t, TFilter, ExtractValues, isDense>(
              *reinterpret_cast<TFilter*>(filter), this, rows, extractValues));
      break;

    case 4:
      reinterpret_cast<Reader*>(this)->Reader::readWithVisitor(
          rows,
          ColumnVisitor<int32_t, TFilter, ExtractValues, isDense>(
              *reinterpret_cast<TFilter*>(filter), this, rows, extractValues));

      break;

    case 8:
      reinterpret_cast<Reader*>(this)->Reader::readWithVisitor(
          rows,
          ColumnVisitor<int64_t, TFilter, ExtractValues, isDense>(
              *reinterpret_cast<TFilter*>(filter), this, rows, extractValues));
      break;
    default:
      VELOX_FAIL("Unsupported valueSize_ {}", valueSize_);
  }
}

template <typename Reader, bool isDense, typename ExtractValues>
void SelectiveIntegerColumnReader::processFilter(
    common::Filter* filter,
    ExtractValues extractValues,
    RowSet rows) {
  switch (filter ? filter->kind() : common::FilterKind::kAlwaysTrue) {
    case common::FilterKind::kAlwaysTrue:
      readHelper<Reader, common::AlwaysTrue, isDense>(
          filter, rows, extractValues);
      break;
    case common::FilterKind::kIsNull:
      filterNulls<int64_t>(
          rows,
          true,
          !std::is_same<decltype(extractValues), DropValues>::value);
      break;
    case common::FilterKind::kIsNotNull:
      if (std::is_same<decltype(extractValues), DropValues>::value) {
        filterNulls<int64_t>(rows, false, false);
      } else {
        readHelper<Reader, common::IsNotNull, isDense>(
            filter, rows, extractValues);
      }
      break;
    case common::FilterKind::kBigintRange:
      readHelper<Reader, common::BigintRange, isDense>(
          filter, rows, extractValues);
      break;
    case common::FilterKind::kBigintValuesUsingHashTable:
      readHelper<Reader, common::BigintValuesUsingHashTable, isDense>(
          filter, rows, extractValues);
      break;
    case common::FilterKind::kBigintValuesUsingBitmask:
      readHelper<Reader, common::BigintValuesUsingBitmask, isDense>(
          filter, rows, extractValues);
      break;
    case common::FilterKind::kNegatedBigintValuesUsingHashTable:
      readHelper<Reader, common::NegatedBigintValuesUsingHashTable, isDense>(
          filter, rows, extractValues);
      break;
    case common::FilterKind::kNegatedBigintValuesUsingBitmask:
      readHelper<Reader, common::NegatedBigintValuesUsingBitmask, isDense>(
          filter, rows, extractValues);
      break;
    default:
      readHelper<Reader, common::Filter, isDense>(filter, rows, extractValues);
      break;
  }
}

template <typename Reader, bool isDense>
void SelectiveIntegerColumnReader::processValueHook(
    RowSet rows,
    ValueHook* hook) {
  switch (hook->kind()) {
    case aggregate::AggregationHook::kSumBigintToBigint:
      readHelper<Reader, common::AlwaysTrue, isDense>(
          &alwaysTrue(),
          rows,
          ExtractToHook<aggregate::SumHook<int64_t, int64_t>>(hook));
      break;
    case aggregate::AggregationHook::kBigintMax:
      readHelper<Reader, common::AlwaysTrue, isDense>(
          &alwaysTrue(),
          rows,
          ExtractToHook<aggregate::MinMaxHook<int64_t, false>>(hook));
      break;
    case aggregate::AggregationHook::kBigintMin:
      readHelper<Reader, common::AlwaysTrue, isDense>(
          &alwaysTrue(),
          rows,
          ExtractToHook<aggregate::MinMaxHook<int64_t, true>>(hook));
      break;
    default:
      readHelper<Reader, common::AlwaysTrue, isDense>(
          &alwaysTrue(), rows, ExtractToGenericHook(hook));
  }
}

template <typename Reader>
void SelectiveIntegerColumnReader::readCommon(RowSet rows) {
  bool isDense = rows.back() == rows.size() - 1;
  common::Filter* filter =
      scanSpec_->filter() ? scanSpec_->filter() : &alwaysTrue();
  if (scanSpec_->keepValues()) {
    if (scanSpec_->valueHook()) {
      if (isDense) {
        processValueHook<Reader, true>(rows, scanSpec_->valueHook());
      } else {
        processValueHook<Reader, false>(rows, scanSpec_->valueHook());
      }
      return;
    }
    if (isDense) {
      processFilter<Reader, true>(filter, ExtractToReader(this), rows);
    } else {
      processFilter<Reader, false>(filter, ExtractToReader(this), rows);
    }
  } else {
    if (isDense) {
      processFilter<Reader, true>(filter, DropValues(), rows);
    } else {
      processFilter<Reader, false>(filter, DropValues(), rows);
    }
  }
}

} // namespace facebook::velox::dwrf
