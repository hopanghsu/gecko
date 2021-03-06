// |reftest| skip-if(release_or_beta) error:SyntaxError -- async-iteration is not released yet
'use strict';
// This file was procedurally generated from the following sources:
// - src/async-generators/yield-identifier-spread-strict.case
// - src/async-generators/default/async-class-expr-method.template
/*---
description: It's an early error if the AssignmentExpression is a function body with yield as an identifier in strict mode. (Async generator method as a ClassExpression element)
esid: prod-AsyncGeneratorMethod
features: [object-spread, async-iteration]
flags: [generated, onlyStrict]
negative:
  phase: early
  type: SyntaxError
info: |
    ClassElement :
      MethodDefinition

    MethodDefinition :
      AsyncGeneratorMethod

    Async Generator Function Definitions

    AsyncGeneratorMethod :
      async [no LineTerminator here] * PropertyName ( UniqueFormalParameters ) { AsyncGeneratorBody }


    Spread Properties

    PropertyDefinition[Yield]:
      (...)
      ...AssignmentExpression[In, ?Yield]

---*/


var callCount = 0;

var C = class { async *gen() {
    callCount += 1;
    return {
         ...(function() {
            var yield;
            throw new Test262Error();
         }()),
      }
}}

var gen = C.prototype.gen;

var iter = gen();



assert.sameValue(callCount, 1);
