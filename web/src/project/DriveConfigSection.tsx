import { appendItem, removeAt, replaceAt } from './edit'
import type { ModbusDeviceDef, ModbusPointDef, ModbusRegisterType, Project } from './types'

const kRegisterTypes: ModbusRegisterType[] = ['Coil', 'DiscreteInput', 'HoldingRegister', 'InputRegister']

function PointsEditor({
  points,
  onChange,
}: {
  points: ModbusPointDef[]
  onChange: (points: ModbusPointDef[]) => void
}) {
  function update(index: number, patch: Partial<ModbusPointDef>) {
    onChange(replaceAt(points, index, { ...points[index], ...patch }))
  }
  return (
    <table>
      <thead>
        <tr>
          <th>Tag</th>
          <th>Register type</th>
          <th>Address</th>
          <th />
        </tr>
      </thead>
      <tbody>
        {points.map((p, i) => (
          <tr key={i}>
            <td>
              <input value={p.tagName} onChange={(e) => update(i, { tagName: e.target.value })} />
            </td>
            <td>
              <select
                value={p.registerType}
                onChange={(e) => update(i, { registerType: e.target.value as ModbusRegisterType })}
              >
                {kRegisterTypes.map((t) => (
                  <option key={t} value={t}>
                    {t}
                  </option>
                ))}
              </select>
            </td>
            <td>
              <input
                type="number"
                min={0}
                value={p.address}
                onChange={(e) => update(i, { address: Number(e.target.value) })}
              />
            </td>
            <td>
              <button onClick={() => onChange(removeAt(points, i))}>Remove</button>
            </td>
          </tr>
        ))}
      </tbody>
      <tfoot>
        <tr>
          <td colSpan={4}>
            <button
              onClick={() =>
                onChange(appendItem(points, { tagName: '', registerType: 'HoldingRegister', address: 0 }))
              }
            >
              + Add point
            </button>
          </td>
        </tr>
      </tfoot>
    </table>
  )
}

// Drive configuration: Modbus TCP/RTU device + point config, matching
// io::ModbusDeviceConfig/ModbusPointMapping in the C++ backend (see
// docs/architecture.md's "Modbus TCP I/O driver" section). Known v1 limitation --
// documented in docs/roadmap.md, not fixed here -- apps/plc_server always runs a
// SimulatedIoDriver; this section is form-editable and stored with the project, but
// doesn't yet get wired into a real IIoDriver at download time. It still shapes the
// generated ST source's addressing (a Modbus point's tag needs an IO-linking entry
// at a matching address to actually be reachable once a real driver exists).
export function DriveConfigSection({
  project,
  onChange,
}: {
  project: Project
  onChange: (project: Project) => void
}) {
  function update(index: number, device: ModbusDeviceDef) {
    onChange({ ...project, driveConfig: replaceAt(project.driveConfig, index, device) })
  }

  function addDevice() {
    const existing = new Set(project.driveConfig.map((d) => d.name))
    let name = 'Drive1'
    let n = 1
    while (existing.has(name)) {
      name = `Drive${++n}`
    }
    onChange({
      ...project,
      driveConfig: appendItem(project.driveConfig, {
        name,
        transport: 'tcp',
        host: '192.168.0.1',
        port: 502,
        unitId: 1,
        points: [],
      }),
    })
  }

  return (
    <div className="drive-config-section">
      <p className="drive-config-section__hint">
        Modbus TCP/RTU device configuration. Not yet wired into a real IIoDriver --
        plc_server still runs on simulated IO (see docs/roadmap.md).
      </p>
      {project.driveConfig.map((device, i) => (
        <div className="drive-config-section__device" key={i}>
          <div className="drive-config-section__header">
            <label>
              Name
              <input value={device.name} onChange={(e) => update(i, { ...device, name: e.target.value })} />
            </label>
            <label>
              Transport
              <select
                value={device.transport}
                onChange={(e) => update(i, { ...device, transport: e.target.value as 'tcp' | 'rtu' })}
              >
                <option value="tcp">TCP</option>
                <option value="rtu">RTU (serial)</option>
              </select>
            </label>
            {device.transport === 'tcp' ? (
              <>
                <label>
                  Host
                  <input
                    value={device.host ?? ''}
                    onChange={(e) => update(i, { ...device, host: e.target.value })}
                  />
                </label>
                <label>
                  Port
                  <input
                    type="number"
                    value={device.port ?? 502}
                    onChange={(e) => update(i, { ...device, port: Number(e.target.value) })}
                  />
                </label>
              </>
            ) : (
              <>
                <label>
                  Device path
                  <input
                    placeholder="/dev/ttyUSB0"
                    value={device.devicePath ?? ''}
                    onChange={(e) => update(i, { ...device, devicePath: e.target.value })}
                  />
                </label>
                <label>
                  Baud
                  <input
                    type="number"
                    value={device.baud ?? 9600}
                    onChange={(e) => update(i, { ...device, baud: Number(e.target.value) })}
                  />
                </label>
              </>
            )}
            <label>
              Unit ID
              <input
                type="number"
                min={0}
                value={device.unitId}
                onChange={(e) => update(i, { ...device, unitId: Number(e.target.value) })}
              />
            </label>
            <button
              onClick={() => onChange({ ...project, driveConfig: removeAt(project.driveConfig, i) })}
            >
              Remove device
            </button>
          </div>
          <PointsEditor points={device.points} onChange={(points) => update(i, { ...device, points })} />
        </div>
      ))}
      <button onClick={addDevice}>+ Add device</button>
    </div>
  )
}
