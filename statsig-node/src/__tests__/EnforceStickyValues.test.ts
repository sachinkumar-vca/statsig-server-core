import 'jest-extended';
import * as fs from 'node:fs';
import * as path from 'node:path';

import {
  PersistentStorage,
  Statsig,
  StatsigUser,
  StickyValues,
} from '../../build/index.js';
import { MockScrapi } from './MockScrapi';

// Covers the enforceOverrides / enforceTargeting persistent-assignment
// options. Fixture (enforce_sticky_dcs.json): experiment `enforce_exp` with a
// console override rule matching userID `override-user`, a targeting gate
// passing only users with custom `targeted=yes`, and layer `enforce_layer`
// delegating to the experiment.

class TestPersistentStorage implements PersistentStorage {
  data: Record<string, Record<string, StickyValues>> = {};

  load = (key: string): Record<string, StickyValues> | null => {
    return this.data[key] ?? null;
  };

  save = (key: string, config_name: string, data: StickyValues): void => {
    this.data[key] = { ...this.data[key], [config_name]: data };
  };

  delete = (key: string, config_name: string): void => {
    const found = this.data[key];
    if (found) {
      delete found[config_name];
    }
  };
}

function stickyValues(
  configName: string,
  configDelegate: string | null,
): Record<string, StickyValues> {
  return {
    [configName]: {
      value: true,
      json_value: { value: 'sticky_value' },
      rule_id: 'sticky_rule_id',
      group_name: 'Sticky Group',
      secondary_exposures: [],
      undelegated_secondary_exposures: [],
      config_delegate: configDelegate,
      explicit_parameters: null,
      time: 1700000000000,
    },
  };
}

function makeUser(userID: string, targeted: boolean): StatsigUser {
  return new StatsigUser({
    userID,
    custom: { targeted: targeted ? 'yes' : 'no' },
  });
}

describe('Enforce Sticky Values', () => {
  let scrapi: MockScrapi;
  let statsig: Statsig;

  beforeAll(async () => {
    scrapi = await MockScrapi.create();

    const dcs = fs.readFileSync(
      path.join(__dirname, 'data/enforce_sticky_dcs.json'),
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

    statsig = new Statsig('secret-123', {
      specsUrl: scrapi.getUrlForPath('/v2/download_config_specs'),
      logEventUrl: scrapi.getUrlForPath('/v1/log_event'),
      // userPersistedValues are only honored when a persistent storage
      // adapter is configured.
      persistentStorage: new TestPersistentStorage(),
      outputLogLevel: 'error',
    });

    await statsig.initialize();
  });

  afterAll(async () => {
    await statsig.shutdown();
    scrapi.close();
  });

  describe('getExperiment', () => {
    it('returns the sticky value when enforceOverrides is off', () => {
      const experiment = statsig.getExperiment(
        makeUser('override-user', true),
        'enforce_exp',
        { userPersistedValues: stickyValues('enforce_exp', null) },
      );

      expect(experiment.getValue('value')).toBe('sticky_value');
      expect(experiment.ruleID).toBe('sticky_rule_id');
    });

    it('returns the override value when enforceOverrides is on and an override rule matches', () => {
      const experiment = statsig.getExperiment(
        makeUser('override-user', true),
        'enforce_exp',
        {
          userPersistedValues: stickyValues('enforce_exp', null),
          enforceOverrides: true,
        },
      );

      expect(experiment.getValue('value')).toBe('override_value');
      expect(experiment.ruleID).toBe('override_rule:userID:id_override');
    });

    it('returns the sticky value when no override rule matches the user', () => {
      const experiment = statsig.getExperiment(
        makeUser('plain-user', true),
        'enforce_exp',
        {
          userPersistedValues: stickyValues('enforce_exp', null),
          enforceOverrides: true,
        },
      );

      expect(experiment.getValue('value')).toBe('sticky_value');
    });

    it('returns the sticky value when the user still passes targeting', () => {
      const experiment = statsig.getExperiment(
        makeUser('plain-user', true),
        'enforce_exp',
        {
          userPersistedValues: stickyValues('enforce_exp', null),
          enforceTargeting: true,
        },
      );

      expect(experiment.getValue('value')).toBe('sticky_value');
    });

    it('drops the sticky value when the user no longer passes targeting', () => {
      const experiment = statsig.getExperiment(
        makeUser('plain-user', false),
        'enforce_exp',
        {
          userPersistedValues: stickyValues('enforce_exp', null),
          enforceTargeting: true,
        },
      );

      expect(experiment.getValue('value')).not.toBe('sticky_value');
      expect(experiment.ruleID).toBe('targetingGate');
    });
  });

  describe('getLayer', () => {
    it('returns the sticky value when enforceOverrides is off', () => {
      const layer = statsig.getLayer(
        makeUser('override-user', true),
        'enforce_layer',
        { userPersistedValues: stickyValues('enforce_layer', 'enforce_exp') },
      );

      expect(layer.getValue('value')).toBe('sticky_value');
    });

    it('returns the override value when enforceOverrides is on and the delegate override matches', () => {
      const layer = statsig.getLayer(
        makeUser('override-user', true),
        'enforce_layer',
        {
          userPersistedValues: stickyValues('enforce_layer', 'enforce_exp'),
          enforceOverrides: true,
        },
      );

      expect(layer.getValue('value')).toBe('override_value');
    });

    it('drops the sticky value when the user no longer passes targeting', () => {
      const layer = statsig.getLayer(
        makeUser('plain-user', false),
        'enforce_layer',
        {
          userPersistedValues: stickyValues('enforce_layer', 'enforce_exp'),
          enforceTargeting: true,
        },
      );

      expect(layer.getValue('value')).not.toBe('sticky_value');
    });
  });
});
