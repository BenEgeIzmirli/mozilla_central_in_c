// |reftest| skip -- BigInt is not supported
// Copyright (C) 2017 Igalia, S.L. All rights reserved.
// This code is governed by the BSD license found in the LICENSE file.

/*---
esid: sec-dataview.prototype.setbigint64
description: >
  Set values and return undefined
includes: [byteConversionValues.js]
features: [DataView, ArrayBuffer, BigInt, arrow-function]
---*/

var buffer = new ArrayBuffer(8);
var sample = new DataView(buffer, 0);

var values = byteConversionValues.values;

values.forEach(function(value, i) {
  if (value === undefined) {
    assert.throws(TypeError,
      () => sample.setBigInt64(0, BigInt(value), false),
      "value: " + value);
    return;
  } else if (!Number.isInteger(value) || value > 9007199254740991) {
    assert.throws(RangeError,
      () => sample.setBigInt64(0, BigInt(value), false),
      "value " + value);
    return;
  }

  var result = sample.setBigInt64(0, BigInt(value), false);

  assert.sameValue(
    sample.getBigInt64(0),
    BigInt(value),
    "value: " + value
  );

  assert.sameValue(
    result,
    undefined,
    "return is undefined, value: " + value
  );
});

reportCompare(0, 0);
