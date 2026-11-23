// Unit tests of the actual shipped controller. GDB transport is mocked here;
// these tests are NOT guest execution or proof of iOS JIT permission.
const fs = require('node:fs');
const vm = require('node:vm');
const assert = require('node:assert/strict');
const source = fs.readFileSync('MaxPS4-iOS/Resources/JIT/maxps4-stik.js', 'utf8');
function run(failAt = '', badAllocation = '') {
    const commands = [], prepared = [];
    let allocation = 0;
    const context = {
        MAXPS4_MAILBOX: 0x100000n, MAXPS4_NONCE: 0x123456789abcdef0n,
        MAXPS4_SIZES: [16384, 134217728, 16384],
        get_pid: () => 42, log: () => {},
        send_command(command) {
            commands.push(command);
            if (command.startsWith(failAt) && failAt) return 'E01';
            if (command.startsWith('vAttach')) return 'T05thread:1;';
            if (command.startsWith('m')) return failAt === 'replay' ? '00'.repeat(40) :
                '00'.repeat(8) + 'f0debc9a78563412' + '00'.repeat(24);
            if (command.startsWith('_M')) return badAllocation || (0x10000000 + allocation++ * 0x2000000).toString(16);
            return 'OK';
        },
        prepare_memory_region(address, length) {
            assert.equal(typeof address, 'number');
            assert.equal(typeof length, 'number');
            prepared.push([address, length]);
            return failAt === 'prepare' ? 'EACCES' : 'OK';
        }
    };
    let error;
    try { vm.runInNewContext(source, context, {timeout: 1000}); } catch (e) { error = e; }
    return {commands, prepared, error};
}
let test = run();
assert.equal(test.error, undefined);
assert.deepEqual(test.prepared.map(x => x[1]), [16384, 134217728, 16384]);
assert.equal(test.commands[0], 'vAttach;2a');
assert.equal(test.commands.at(-1), 'D');
const writes = test.commands.filter(x => x.startsWith('M'));
assert.equal(writes[0], 'M100008,20:0100000000000000000000100000000000000012000000000000001400000000');
assert.equal(writes[1], 'M100000,8:f0debc9a78563412');
assert(!test.commands.some(x => /^P|^c$/.test(x)), 'No application breakpoint or register overwrite');
for (const stage of ['vAttach', 'm100000', 'replay', '_M', 'prepare', 'M100008', 'M100000', 'D']) {
    test = run(stage);
    if (stage === 'D') assert(test.error);
    else assert.equal(test.error, undefined);
    if (stage === 'vAttach') assert.equal(test.commands.length, 1);
    else assert.equal(test.commands.at(-1), 'D');
    if (stage === 'prepare') assert(test.commands.includes('_m10000000'));
    if (stage === '_M') assert.equal(test.prepared.length, 0);
    if (stage === 'm100000' || stage === 'replay') {
        assert.equal(test.prepared.length, 0);
        assert(!test.commands.some(x => x.startsWith('M')), 'A stale/unreadable challenge must never be overwritten');
    }
    if (stage === 'prepare' || stage === '_M') {
        assert(test.commands.includes('M100008,20:0200000000000000' + '00'.repeat(24)));
        assert(!test.commands.includes(writes[0]), 'Failure must never publish successful RX metadata');
    }
}
for (const address of ['0', '1234', '20000000000000', 'E22', 'no-memory']) {
    test = run('', address);
    assert.equal(test.prepared.length, 0);
    assert(!test.commands.includes(writes[0]));
}
console.log('PASS shipped Stik controller: exact mailbox/GDB contract, failure cleanup, no false readiness');
