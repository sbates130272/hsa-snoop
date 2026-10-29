# Perfetto Trace Visualization

`hsa-snoop` emits native Perfetto traces that can be opened in
[Perfetto UI](https://ui.perfetto.dev/). Each AQL or SDMA queue gets its own
timeline track; selecting a slice exposes decoded metadata including grid
dimensions, kernel object, segment sizes and completion signals.

<table>
  <tr>
    <td width="50%">
      <a href="website/images/perfetto-trace-overview.png">
        <img src="website/images/perfetto-trace-overview.png"
             alt="Overview of an hsa-snoop trace in Perfetto"
             width="100%">
      </a>
    </td>
    <td width="50%">
      <a href="website/images/perfetto-dispatch-detail.png">
        <img src="website/images/perfetto-dispatch-detail.png"
             alt="Detailed kernel dispatch metadata in Perfetto"
             width="100%">
      </a>
    </td>
  </tr>
  <tr>
    <td align="center">
      <sub>Full trace showing packet activity across two AQL queues.</sub>
    </td>
    <td align="center">
      <sub>Kernel dispatches and barrier packets, with decoded dispatch metadata.</sub>
    </td>
  </tr>
</table>

> [!NOTE]
> `hsa-snoop` slices represent approximate enqueue-to-queue-consumed residency,
> not GPU execution time.

Output format is native Perfetto protobuf (`.pftrace`, default) or Chrome JSON
(`--format json`); both open in <https://ui.perfetto.dev>.
