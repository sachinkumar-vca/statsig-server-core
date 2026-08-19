import * as fs from 'node:fs';
import * as path from 'node:path';

import { Statsig, StatsigOptions } from '../../build/index.js';
import { MockScrapi } from './MockScrapi';

describe('Get Layer List', () => {
  let scrapi: MockScrapi;
  let statsig: Statsig;

  beforeAll(async () => {
    scrapi = await MockScrapi.create();

    const dcs = fs.readFileSync(
      path.join(
        __dirname,
        '../../../statsig-rust/tests/data/eval_proj_dcs.json',
      ),
      'utf8',
    );

    scrapi.mock('/v2/download_config_specs', dcs, {
      status: 200,
      method: 'GET',
    });

    scrapi.mock('/v1/log_event', '{"success": true}', {
      status: 202,
      method: 'POST',
    });

    const specsUrl = scrapi.getUrlForPath('/v2/download_config_specs');
    const logEventUrl = scrapi.getUrlForPath('/v1/log_event');
    const options: StatsigOptions = {
      specsUrl,
      logEventUrl,
    };

    statsig = new Statsig('secret-123', options);
    await statsig.initialize();
  });

  afterAll(async () => {
    await statsig.shutdown();
    scrapi.close();
  });

  it('returns the configured layer names', () => {
    const layerList = statsig.getLayerList();

    expect(Array.isArray(layerList)).toBe(true);
    expect(layerList).toContain('test_layer');
  });
});
