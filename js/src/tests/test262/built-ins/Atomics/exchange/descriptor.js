// |reftest| skip-if(!this.hasOwnProperty('Atomics')) -- Atomics is not enabled unconditionally
// Copyright 2015 Microsoft Corporation. All rights reserved.
// Copyright (C) 2017 Mozilla Corporation. All rights reserved.
// This code is governed by the license found in the LICENSE file.

/*---
esid: sec-atomics.exchange
description: Testing descriptor property of Atomics.exchange
includes: [propertyHelper.js]
features: [Atomics]
---*/

verifyWritable(Atomics, "exchange");
verifyNotEnumerable(Atomics, "exchange");
verifyConfigurable(Atomics, "exchange");

reportCompare(0, 0);
