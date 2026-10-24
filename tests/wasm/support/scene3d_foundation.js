// SPDX-License-Identifier: MPL-2.0
const { expect } = require('@playwright/test');

async function requestScene3DFoundationState(page, consoleMessages, previousCount) {
  await page.evaluate(() => window.pjWasmReportScene3DFoundationProbe());
  await expect.poll(
    () => consoleMessages.filter(
      message => message.includes('PJ_WASM_SCENE3D_FOUNDATION_SUMMARY'),
    ).length,
    { timeout: 10000 },
  ).toBeGreaterThan(previousCount);

  const summaryMessage = consoleMessages.filter(
    message => message.includes('PJ_WASM_SCENE3D_FOUNDATION_SUMMARY'),
  ).at(-1) || '';
  const summary = summaryMessage.match(
    /sequence=(\d+) docks=(\d+) slider=(-?\d+),(-?\d+),(\d+)x(\d+) value=([^ ]+) range=([^,]+),([^ ]+) enabled=(\d+) window=(\d+)x(\d+)/,
  );
  expect(summary, `unparseable Scene3D summary: ${summaryMessage}`).not.toBeNull();
  const sequence = Number(summary[1]);
  const dockCount = Number(summary[2]);
  const dockMessages = () => consoleMessages.filter(
    message => message.includes('PJ_WASM_SCENE3D_FOUNDATION_DOCK')
      && message.includes(`sequence=${sequence} `),
  );
  await expect.poll(() => dockMessages().length).toBe(dockCount);
  const dataMessages = () => consoleMessages.filter(
    message => message.includes('PJ_WASM_SCENE3D_DATA')
      && message.includes(`sequence=${sequence} `),
  );
  await expect.poll(() => dataMessages().length).toBe(dockCount);
  const dataByIndex = new Map(dataMessages().map((message) => {
    const match = message.match(
      /index=(\d+) points=(\d+),(\d+),(\d+),(\d+),(\d+),(\d+),(\d+) compressed=(\d+) depth=(\d+),(\d+),(\d+),(\d+),(\d+),(\d+) poses=(\d+),(\d+) occupancy=(\d+),(\d+),(\d+),(\d+) voxels=(\d+),(\d+),(\d+),(\d+) trails=(\d+),(\d+) order=([^ ]*) submitted=([^ ]*) list=(-?\d+),(-?\d+),(\d+)x(\d+) rows=([^ ]*) toggle=(-?\d+),(-?\d+),(\d+)x(\d+) right_visible=(\d+) warnings=([^ ]*)/,
    );
    expect(match, `unparseable Scene3D data state: ${message}`).not.toBeNull();
    const ids = encoded => decodeURIComponent(encoded).split(',').filter(Boolean).map(Number);
    return [Number(match[1]), {
      points: {
        live: Number(match[2]), rendered: Number(match[3]), vertices: Number(match[4]),
        starts: Number(match[5]), completed: Number(match[6]), offMain: Number(match[7]),
        decoding: Number(match[8]), compressed: Number(match[9]),
      },
      depth: {
        live: Number(match[10]), decoding: Number(match[11]), starts: Number(match[12]),
        completed: Number(match[13]), offMain: Number(match[14]), vertices: Number(match[15]),
      },
      poses: { live: Number(match[16]), arms: Number(match[17]) },
      occupancy: {
        live: Number(match[18]), cells: Number(match[19]),
        fullUploads: Number(match[20]), partialUploads: Number(match[21]),
      },
      voxels: {
        live: Number(match[22]), count: Number(match[23]), uploads: Number(match[24]), max3d: Number(match[25]),
      },
      trails: { rendered: Number(match[26]), vertices: Number(match[27]) },
      order: ids(match[28]),
      submitted: ids(match[29]),
      list: { x: Number(match[30]), y: Number(match[31]), width: Number(match[32]), height: Number(match[33]) },
      rows: decodeURIComponent(match[34]).split(',').filter(Boolean).map((entry) => {
        const [id, x, y] = entry.split(':').map(Number);
        return { id, x, y };
      }),
      rightPanel: {
        toggle: {
          x: Number(match[35]), y: Number(match[36]), width: Number(match[37]), height: Number(match[38]),
        },
        visible: match[39] === '1',
      },
      warnings: decodeURIComponent(match[40]).trim().split('|').filter(Boolean),
    }];
  }));
  const modelMessages = () => consoleMessages.filter(
    message => message.includes('PJ_WASM_SCENE3D_MODELS')
      && message.includes(`sequence=${sequence} `),
  );
  await expect.poll(() => modelMessages().length).toBe(dockCount);
  const modelsByIndex = new Map(modelMessages().map((message) => {
    const fields = Object.fromEntries(
      [...message.matchAll(/(?:^| )([a-z_]+)=([^ ]*)/g)].map(match => [match[1], match[2]]),
    );
    const numbers = (name, count) => {
      const values = (fields[name] || '').split(',').map(Number);
      expect(values, `invalid ${name} state: ${message}`).toHaveLength(count);
      expect(values.every(Number.isFinite), `non-numeric ${name} state: ${message}`).toBe(true);
      return values;
    };
    const markers = numbers('markers', 8);
    const families = numbers('families', 7);
    const skipped = numbers('skipped', 3);
    const model = numbers('models', 6);
    const maps = numbers('maps', 5);
    const robots = numbers('robots', 10);
    return [Number(fields.index), {
      markers: {
        live: markers[0], decoding: markers[1], starts: markers[2],
        completed: markers[3], offMain: markers[4], rendered: markers[5],
        instances: markers[6], stream: markers[7],
        families: {
          cubes: families[0], spheres: families[1], cylinders: families[2],
          arrows: families[3], axes: families[4],
          lineVertices: families[5], triangleVertices: families[6],
        },
        skipped: { text: skipped[0], models: skipped[1], invalid: skipped[2] },
      },
      models: {
        ready: model[0], live: model[1], bytes: model[2],
        rendered: model[3], draws: model[4], triangles: model[5],
        maps: {
          baseColor: maps[0], metallicRoughness: maps[1], normal: maps[2],
          occlusion: maps[3], emissive: maps[4],
        },
      },
      robots: {
        live: robots[0], meshes: robots[1], draws: robots[2],
        visual: robots[3], collision: robots[4], placeholders: robots[5],
        bridges: robots[6], sourceType: robots[7], displayMode: robots[8],
        revision: robots[9], source: decodeURIComponent(fields.source),
        status: decodeURIComponent(fields.status).split('|').filter(Boolean),
        sourceIndex: Number(fields.source_index),
      },
    }];
  }));

  const qualityMessages = () => consoleMessages.filter(
    message => message.includes('PJ_WASM_SCENE3D_QUALITY')
      && message.includes(`sequence=${sequence} `),
  );
  await expect.poll(() => qualityMessages().length).toBe(dockCount);
  const qualityByIndex = new Map(qualityMessages().map((message) => {
    const fields = Object.fromEntries(
      [...message.matchAll(/(?:^| )([a-z_]+)=([^ ]*)/g)].map(match => [match[1], match[2]]),
    );
    const quality = (fields.quality || '').split(',').map(Number);
    expect(quality, `invalid quality state: ${message}`).toHaveLength(19);
    expect(quality.every(Number.isFinite), `non-numeric quality state: ${message}`).toBe(true);
    const errors = decodeURIComponent(fields.errors || '').split('|');
    return [Number(fields.index), {
      hdr: {
        active: quality[0] === 1, ready: quality[1] === 1,
        width: quality[2], height: quality[3], bytes: quality[4], generation: quality[5],
        error: errors[0] || '',
      },
      ssao: {
        active: quality[6] === 1, ready: quality[7] === 1, generation: quality[8],
        error: errors[1] || '',
      },
      edl: {
        active: quality[9] === 1, ready: quality[10] === 1, generation: quality[11],
        error: errors[2] || '',
      },
      shadow: {
        active: quality[12] === 1, ready: quality[13] === 1, fit: quality[14] === 1,
        size: quality[15], draws: quality[16], triangles: quality[17],
        generation: quality[18], error: errors[3] || '',
      },
    }];
  }));

  const docks = dockMessages().map((message) => {
    const match = message.match(
      /index=(\d+) identity=(\d+) view=(-?\d+),(-?\d+),(\d+)x(\d+) visible=(\d+) frames=([^ ]*) fixed=([^ ]*) auto=(\d+) follow=([^ ]*) model=(-?\d+) camera=([^,]+),([^,]+),([^,]+),([^,]+),([^,]+),([^,]+),([^ ]+) vertices=(\d+),(\d+) resolved=(\d+) fixed_control=(-?\d+),(-?\d+),(\d+)x(\d+) camera_control=(-?\d+),(-?\d+),(\d+)x(\d+) home=(-?\d+),(-?\d+),(\d+)x(\d+) frame_hash=(\d+) rgb_sum=(\d+) framebuffer=(\d+)x(\d+)/,
    );
    expect(match, `unparseable Scene3D dock: ${message}`).not.toBeNull();
    return {
      index: Number(match[1]),
      identity: Number(match[2]),
      view: {
        x: Number(match[3]), y: Number(match[4]),
        width: Number(match[5]), height: Number(match[6]),
      },
      visible: match[7] === '1',
      frames: decodeURIComponent(match[8]).split(',').filter(Boolean),
      fixed: decodeURIComponent(match[9]),
      auto: match[10] === '1',
      follow: decodeURIComponent(match[11]),
      model: Number(match[12]),
      camera: {
        focalX: Number(match[13]), focalY: Number(match[14]), focalZ: Number(match[15]),
        radius: Number(match[16]), azimuth: Number(match[17]), elevation: Number(match[18]),
        orthoScale: Number(match[19]),
      },
      vertices: { lines: Number(match[20]), triangles: Number(match[21]) },
      resolved: Number(match[22]),
      controls: {
        fixed: {
          x: Number(match[23]), y: Number(match[24]),
          width: Number(match[25]), height: Number(match[26]),
        },
        camera: {
          x: Number(match[27]), y: Number(match[28]),
          width: Number(match[29]), height: Number(match[30]),
        },
        home: {
          x: Number(match[31]), y: Number(match[32]),
          width: Number(match[33]), height: Number(match[34]),
        },
      },
      readback: {
        frameHash: match[35],
        rgbSum: Number(match[36]),
        width: Number(match[37]),
        height: Number(match[38]),
      },
      data: dataByIndex.get(Number(match[1])),
      models: {
        ...modelsByIndex.get(Number(match[1])),
        quality: qualityByIndex.get(Number(match[1])),
      },
    };
  });

  return {
    sequence,
    window: { width: Number(summary[11]), height: Number(summary[12]) },
    slider: {
      x: Number(summary[3]), y: Number(summary[4]),
      width: Number(summary[5]), height: Number(summary[6]),
      value: Number(summary[7]), min: Number(summary[8]), max: Number(summary[9]),
      enabled: summary[10] === '1',
    },
    docks,
  };
}

function qtPointToCss(screen, state, point) {
  return {
    x: screen.x + (point.x * screen.width / state.window.width),
    y: screen.y + (point.y * screen.height / state.window.height),
  };
}

module.exports = { qtPointToCss, requestScene3DFoundationState };
