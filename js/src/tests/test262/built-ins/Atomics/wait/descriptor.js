// |reftest| skip-if(!this.hasOwnProperty('Atomics')) -- Atomics is not enabled unconditionally
// Copyright 2015 Microsoft Corporation. All rights reserved.
// Copyright (C) 2017 Mozilla Corporation. All rights reserved.
// This code is governed by the license found in the LICENSE file.

/*---
esid: sec-atomics.wait
description: Testing descriptor property of Atomics.wait
includes: [propertyHelper.js]
features: [Atomics]
---*/

verifyWritable(Atomics, "wait");
verifyNotEnumerable(Atomics, "wait");
verifyConfigurable(Atomics, "wait");

reportCompare(0, 0);
