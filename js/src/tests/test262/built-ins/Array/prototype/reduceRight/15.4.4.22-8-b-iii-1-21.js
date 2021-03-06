// Copyright (c) 2012 Ecma International.  All rights reserved.
// This code is governed by the BSD license found in the LICENSE file.

/*---
es5id: 15.4.4.22-8-b-iii-1-21
description: >
    Array.prototype.reduceRight - element to be retrieved is inherited
    accessor property without a get function on an Array-like object
---*/

        var testResult = false;
        function callbackfn(prevVal, curVal, idx, obj) {
            if (idx === 1) {
                testResult = (typeof prevVal === "undefined");
            }
        }

        var proto = { 0: 0, 1: 1 };

        Object.defineProperty(proto, "2", {
            set: function () { },
            configurable: true
        });

        var Con = function () { };
        Con.prototype = proto;

        var child = new Con();
        child.length = 3;

        Array.prototype.reduceRight.call(child, callbackfn);

assert(testResult, 'testResult !== true');

reportCompare(0, 0);
