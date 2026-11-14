import sys, io, numpy as np, pyarrow as pa, pyarrow.parquet as pq
from asammdf import MDF, Signal
from mcap.writer import Writer
out = sys.argv[1]
t = [1.0, 2.0, 3.0]; v = [1.5, 2.5, 4.5]
# Parquet: snappy on one column, zstd on the other.
pq.write_table(pa.table({'time': t, 'value': v}), f'{out}/sample.parquet',
               compression={'time': 'snappy', 'value': 'zstd'})
# MF4 4.10 with one float channel.
m = MDF(version='4.10')
m.append([Signal(np.array(v), np.array(t), name='value', unit='m')], comment='pj wasm fixture')
m.save(f'{out}/sample.mf4', overwrite=True)
# MCAP with arrow-ipc messages: one IPC stream per message.
with open(f'{out}/arrow.mcap', 'wb') as f:
    w = Writer(f); w.start()
    sid = w.register_schema(name='sensor', encoding='', data=b'')
    cid = w.register_channel(topic='/sensor', message_encoding='arrow-ipc', schema_id=sid)
    for ti, vi in zip(t, v):
        batch = pa.record_batch({'value': pa.array([vi])})
        sink = io.BytesIO()
        with pa.ipc.new_stream(sink, batch.schema) as s: s.write_batch(batch)
        ns = int(ti * 1e9)
        w.add_message(channel_id=cid, log_time=ns, publish_time=ns, data=sink.getvalue())
    w.finish()
