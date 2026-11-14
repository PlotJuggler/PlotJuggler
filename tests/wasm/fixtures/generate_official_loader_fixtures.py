import sys, io, numpy as np, pyarrow as pa, pyarrow.parquet as pq
from asammdf import MDF, Signal
from mcap.writer import Writer
out = sys.argv[1]
# pj-official-plugins checkout, for the NavMap parser's CDR test message.
plugins = sys.argv[2]
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
# MCAP with InfluxDB line protocol messages, each line carrying its own time.
with open(f'{out}/influx.mcap', 'wb') as f:
    w = Writer(f); w.start()
    sid = w.register_schema(name='telegraf', encoding='', data=b'')
    cid = w.register_channel(topic='/telegraf', message_encoding='influx', schema_id=sid)
    for ti, vi in zip(t, v):
        ns = int(ti * 1e9)
        w.add_message(channel_id=cid, log_time=ns, publish_time=ns,
                      data=f'speed,host=car value={vi} {ns}\n'.encode())
    w.finish()
# MCAP with one NavMap message: the parser's own CDR test message and schema.
data = f'{plugins}/parser_navmap/tests/data'
with open(f'{out}/navmap.mcap', 'wb') as f:
    w = Writer(f); w.start()
    sid = w.register_schema(name='navmap_ros_interfaces/msg/NavMap', encoding='ros2msg',
                            data=open(f'{data}/navmap_current.msg', 'rb').read())
    cid = w.register_channel(topic='/navmap', message_encoding='cdr', schema_id=sid)
    ns = 12_500_000_000
    w.add_message(channel_id=cid, log_time=ns, publish_time=ns,
                  data=open(f'{data}/navmap_current.cdr', 'rb').read())
    w.finish()
