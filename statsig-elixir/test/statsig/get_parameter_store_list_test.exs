defmodule Statsig.GetParameterStoreListTest do
  use ExUnit.Case, async: false

  alias Statsig.Test.MockScrapi
  alias Statsig.Options

  @eval_proj_path Path.expand("../../../statsig-rust/tests/data/eval_proj_dcs.json", __DIR__)
  @eval_proj_data File.read!(@eval_proj_path)

  setup do
    {:ok, mock} = MockScrapi.start_link(@eval_proj_data)

    options = %Options{
      specs_url: mock.specs_url,
      disable_all_logging: true,
      output_log_level: "warn"
    }

    {:ok, _pid} = Statsig.start_link("secret-key", options)
    Statsig.initialize()

    on_exit(fn ->
      Statsig.shutdown()

      if pid = Process.whereis(Statsig) do
        Process.exit(pid, :normal)
      end

      MockScrapi.stop(mock)
    end)

    :ok
  end

  test "returns the parameter store names from the loaded specs" do
    {:ok, result} = Statsig.get_parameter_store_list()

    assert "test_parameter_store" in result
  end
end
