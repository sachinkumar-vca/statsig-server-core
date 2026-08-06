defmodule Statsig.EnforceStickyValuesTest do
  # The SDK runs as a named singleton process, so these tests cannot run
  # concurrently with other suites that start Statsig.
  use ExUnit.Case, async: false

  alias Statsig.ExperimentEvaluationOptions
  alias Statsig.LayerEvaluationOptions
  alias Statsig.Layer
  alias Statsig.Options
  alias Statsig.Test.MockScrapi
  alias Statsig.User

  @moduledoc """
  Covers the enforce_overrides / enforce_targeting persistent-assignment
  options. Fixture (enforce_sticky_dcs.json): experiment `enforce_exp` with a
  console override rule matching userID `override-user`, a targeting gate
  passing only users with custom `targeted=yes`, and layer `enforce_layer`
  delegating to the experiment.
  """

  defmodule ForwardingStorage do
    @moduledoc false
    @behaviour Statsig.PersistentStorage

    @impl true
    def init(test_pid), do: {:ok, test_pid}

    @impl true
    def load(_key, test_pid), do: {:ok, nil, test_pid}

    @impl true
    def handle_save(key, config_name, sticky_values, test_pid) do
      send(test_pid, {:storage_save, key, config_name, sticky_values})
      {:ok, test_pid}
    end

    @impl true
    def handle_delete(key, config_name, test_pid) do
      send(test_pid, {:storage_delete, key, config_name})
      {:ok, test_pid}
    end
  end

  @enforce_dcs File.read!(Path.expand("../data/enforce_sticky_dcs.json", __DIR__))

  setup do
    {:ok, mock_scrapi} = MockScrapi.start_link(@enforce_dcs)

    {:ok, storage_ref} = Statsig.PersistentStorage.start_link(ForwardingStorage, self())

    options = %Options{
      specs_url: mock_scrapi.specs_url,
      disable_all_logging: true,
      # user_persisted_values are only honored when persistent storage is
      # configured.
      persistent_storage: storage_ref
    }

    {:ok, _pid} = Statsig.start_link("secret-key", options)
    Statsig.initialize()

    on_exit(fn ->
      Statsig.shutdown()

      if pid = Process.whereis(Statsig) do
        Process.exit(pid, :normal)
      end

      # The bridge is linked to the test process and usually exits with it
      # before on_exit runs.
      if Process.alive?(storage_ref.pid) do
        Statsig.PersistentStorage.stop(storage_ref)
      end

      MockScrapi.stop(mock_scrapi)
    end)

    :ok
  end

  defp make_user(user_id, targeted) do
    %User{user_id: user_id, custom: %{"targeted" => if(targeted, do: "yes", else: "no")}}
  end

  defp sticky_values(config_name, config_delegate) do
    %{
      config_name => %{
        "value" => true,
        "json_value" => %{"value" => "sticky_value"},
        "rule_id" => "sticky_rule_id",
        "group_name" => "Sticky Group",
        "secondary_exposures" => [],
        "undelegated_secondary_exposures" => [],
        "config_delegate" => config_delegate,
        "explicit_parameters" => nil,
        "time" => 1_700_000_000_000
      }
    }
  end

  test "sticky value wins without enforce_overrides" do
    {:ok, experiment} =
      Statsig.get_experiment(
        "enforce_exp",
        make_user("override-user", true),
        %ExperimentEvaluationOptions{
          user_persisted_values: sticky_values("enforce_exp", nil)
        }
      )

    assert experiment.value["value"] == "sticky_value"
    assert experiment.rule_id == "sticky_rule_id"
  end

  test "enforce_overrides lets an override rule win over the sticky value" do
    {:ok, experiment} =
      Statsig.get_experiment(
        "enforce_exp",
        make_user("override-user", true),
        %ExperimentEvaluationOptions{
          user_persisted_values: sticky_values("enforce_exp", nil),
          enforce_overrides: true
        }
      )

    assert experiment.value["value"] == "override_value"
    assert experiment.rule_id == "override_rule:userID:id_override"
  end

  test "enforce_overrides keeps the sticky value when no override rule matches" do
    {:ok, experiment} =
      Statsig.get_experiment(
        "enforce_exp",
        make_user("plain-user", true),
        %ExperimentEvaluationOptions{
          user_persisted_values: sticky_values("enforce_exp", nil),
          enforce_overrides: true
        }
      )

    assert experiment.value["value"] == "sticky_value"
  end

  test "enforce_targeting keeps the sticky value when the user still passes targeting" do
    {:ok, experiment} =
      Statsig.get_experiment(
        "enforce_exp",
        make_user("plain-user", true),
        %ExperimentEvaluationOptions{
          user_persisted_values: sticky_values("enforce_exp", nil),
          enforce_targeting: true
        }
      )

    assert experiment.value["value"] == "sticky_value"
  end

  test "enforce_targeting drops the sticky value when the user no longer passes targeting" do
    {:ok, experiment} =
      Statsig.get_experiment(
        "enforce_exp",
        make_user("plain-user", false),
        %ExperimentEvaluationOptions{
          user_persisted_values: sticky_values("enforce_exp", nil),
          enforce_targeting: true
        }
      )

    refute experiment.value["value"] == "sticky_value"
    assert experiment.rule_id == "targetingGate"
  end

  test "layer sticky value wins without enforce_overrides" do
    {:ok, layer} =
      Statsig.get_layer(
        "enforce_layer",
        make_user("override-user", true),
        %LayerEvaluationOptions{
          user_persisted_values: sticky_values("enforce_layer", "enforce_exp")
        }
      )

    {:ok, value} = Layer.get(layer, "value", "fallback")
    assert value == "sticky_value"
  end

  test "layer enforce_overrides lets the delegate override win over the sticky value" do
    {:ok, layer} =
      Statsig.get_layer(
        "enforce_layer",
        make_user("override-user", true),
        %LayerEvaluationOptions{
          user_persisted_values: sticky_values("enforce_layer", "enforce_exp"),
          enforce_overrides: true
        }
      )

    {:ok, value} = Layer.get(layer, "value", "fallback")
    assert value == "override_value"
  end

  test "layer enforce_overrides keeps the sticky value when no override rule matches" do
    {:ok, layer} =
      Statsig.get_layer("enforce_layer", make_user("plain-user", true), %LayerEvaluationOptions{
        user_persisted_values: sticky_values("enforce_layer", "enforce_exp"),
        enforce_overrides: true
      })

    {:ok, value} = Layer.get(layer, "value", "fallback")
    assert value == "sticky_value"
  end

  test "layer enforce_targeting keeps the sticky value when the user still passes targeting" do
    {:ok, layer} =
      Statsig.get_layer("enforce_layer", make_user("plain-user", true), %LayerEvaluationOptions{
        user_persisted_values: sticky_values("enforce_layer", "enforce_exp"),
        enforce_targeting: true
      })

    {:ok, value} = Layer.get(layer, "value", "fallback")
    assert value == "sticky_value"
  end

  test "layer enforce_targeting drops the sticky value when the user no longer passes targeting" do
    {:ok, layer} =
      Statsig.get_layer("enforce_layer", make_user("plain-user", false), %LayerEvaluationOptions{
        user_persisted_values: sticky_values("enforce_layer", "enforce_exp"),
        enforce_targeting: true
      })

    # The live evaluation lands on the targeting-gate rule, which carries no
    # layer parameters, so the caller-provided fallback comes back.
    {:ok, value} = Layer.get(layer, "value", "fallback")
    assert value == "fallback"
  end

  test "saves a new sticky value through the persistent storage bridge" do
    # No pre-existing sticky value: the user lands in the experiment group,
    # so the SDK saves a new sticky value via the bridge process.
    {:ok, _experiment} =
      Statsig.get_experiment(
        "enforce_exp",
        make_user("plain-user", true),
        %ExperimentEvaluationOptions{
          user_persisted_values: %{}
        }
      )

    assert_receive {:storage_save, key, "enforce_exp", sticky_values}, 2_000

    assert key ==
             Statsig.PersistentStorage.storage_key(%User{user_id: "plain-user"}, "userID")

    assert is_map(sticky_values)
    assert sticky_values["json_value"]["value"] == "live_value"
    assert sticky_values["value"] == true
  end

  test "deletes the sticky value when no persisted values are provided" do
    {:ok, _experiment} =
      Statsig.get_experiment(
        "enforce_exp",
        make_user("plain-user", true),
        %ExperimentEvaluationOptions{
          user_persisted_values: nil
        }
      )

    assert_receive {:storage_delete, "plain-user:userID", "enforce_exp"}, 2_000
  end
end
