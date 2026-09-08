const assert = require('node:assert/strict');
const { compileEnglishQuery } = require('./naturalLanguageQuery');
const { applyPipeline } = require('./queryCapabilities');

assert.deepEqual(
  compileEnglishQuery('first 5 users where age greater than 18 sort by name'),
  {
    operation: 'find',
    query: { age: { $gt: 18 } },
    limit: 5,
    sort: { name: 1 },
    readOnly: true,
    interpretation: { filters: 1, sortedBy: 'name' },
  }
);
assert.deepEqual(
  applyPipeline([{ active: true }, { active: false }], [
    { $match: { active: true } }, { $count: 'total' }
  ]).documents,
  [{ total: 1 }]
);
console.log('natural query and query helpers passed');
