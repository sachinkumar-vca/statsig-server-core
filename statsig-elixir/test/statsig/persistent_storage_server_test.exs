defmodule Statsig.PersistentStorage.ServerTest do
  use ExUnit.Case, async: true

  alias Statsig.PersistentStorage
  alias Statsig.User

  @moduletag capture_log: true

  @request_tag :statsig_persistent_storage_request

  defmodule TestStore do
    @behaviour Statsig.PersistentStorage

    @impl true
    def init({test_pid, values}), do: {:ok, %{test_pid: test_pid, values: values}}

    @impl true
    def load("boom:" <> _rest, _state), do: raise("load exploded")
    def load("bad-return:" <> _rest, _state), do: :nonsense

    def load(key, state) do
      {:ok, Map.get(state.values, key), state}
    end

    @impl true
    def handle_save("raise", _config_name, _sticky_values, _state) do
      raise "save exploded"
    end

    def handle_save("error", _config_name, _sticky_values, _state) do
      {:error, :storage_unavailable}
    end

    def handle_save("bad-return", _config_name, _sticky_values, _state) do
      :nonsense
    end

    def handle_save(key, config_name, sticky_values, state) do
      send(state.test_pid, {:saved, key, config_name, sticky_values})
      {:ok, state}
    end

    @impl true
    def handle_delete(key, config_name, state) do
      send(state.test_pid, {:deleted, key, config_name})
      {:ok, state}
    end
  end

  defmodule SaveOnlyStore do
    @behaviour Statsig.PersistentStorage

    @impl true
    def init(test_pid), do: {:ok, test_pid}

    @impl true
    def load(_key, test_pid), do: {:ok, nil, test_pid}

    @impl true
    def handle_save(key, config_name, sticky_values, test_pid) do
      send(test_pid, {:saved, key, config_name, sticky_values})
      {:ok, test_pid}
    end
  end

  defmodule RaisingInitStore do
    @behaviour Statsig.PersistentStorage

    @impl true
    def init(_arg), do: raise("init exploded")

    @impl true
    def load(_key, state), do: {:ok, nil, state}

    @impl true
    def handle_save(_key, _config_name, _sticky_values, state), do: {:ok, state}
  end

  defp start_store(values \\ %{}) do
    {:ok, ref} = PersistentStorage.start_link(TestStore, {self(), values})
    ref
  end

  test "forwards save and delete notifications" do
    ref = start_store()
    sticky = %{"value" => true, "json_value" => %{"param" => "a"}}

    send(ref.pid, {@request_tag, :save, "user-1:userID", "my_exp", sticky})
    assert_receive {:saved, "user-1:userID", "my_exp", ^sticky}

    send(ref.pid, {@request_tag, :delete, "user-1:userID", "my_exp"})
    assert_receive {:deleted, "user-1:userID", "my_exp"}
  end

  test "a raising handle_save does not crash the bridge" do
    ref = start_store()

    send(ref.pid, {@request_tag, :save, "raise", "my_exp", %{}})
    send(ref.pid, {@request_tag, :save, "user-1:userID", "my_exp", %{}})

    assert_receive {:saved, "user-1:userID", "my_exp", %{}}
    assert Process.alive?(ref.pid)
  end

  test "error and unexpected returns from handle_save keep the bridge serving" do
    ref = start_store()

    send(ref.pid, {@request_tag, :save, "error", "my_exp", %{}})
    send(ref.pid, {@request_tag, :save, "bad-return", "my_exp", %{}})
    send(ref.pid, {@request_tag, :save, "user-1:userID", "my_exp", %{}})

    assert_receive {:saved, "user-1:userID", "my_exp", %{}}
    assert Process.alive?(ref.pid)
  end

  test "delete is a no-op when handle_delete is not implemented" do
    {:ok, ref} = PersistentStorage.start_link(SaveOnlyStore, self())

    send(ref.pid, {@request_tag, :delete, "user-1:userID", "my_exp"})
    send(ref.pid, {@request_tag, :save, "user-1:userID", "my_exp", %{}})

    assert_receive {:saved, "user-1:userID", "my_exp", %{}}
    assert Process.alive?(ref.pid)
  end

  test "get_values_for_user loads values by derived storage key" do
    values = %{"user-1:userID" => %{"my_exp" => %{"value" => true}}}
    ref = start_store(values)

    assert {:ok, %{"my_exp" => %{"value" => true}}} =
             PersistentStorage.get_values_for_user(ref, %User{user_id: "user-1"})

    assert {:ok, nil} =
             PersistentStorage.get_values_for_user(ref, %User{user_id: "unknown"})
  end

  test "get_values_for_user surfaces load failures as errors" do
    ref = start_store()

    assert {:error, "load exploded"} =
             PersistentStorage.get_values_for_user(ref, %User{user_id: "boom"})

    assert {:error, {:bad_return, _}} =
             PersistentStorage.get_values_for_user(ref, %User{user_id: "bad-return"})

    assert Process.alive?(ref.pid)
  end

  test "a raising init fails start_link instead of crashing the caller" do
    Process.flag(:trap_exit, true)

    assert {:error, "init exploded"} =
             PersistentStorage.start_link(RaisingInitStore, nil)
  end

  test "storage_key mirrors the SDK key format" do
    user = %User{user_id: "user-1", custom_ids: %{"stableID" => "stable-1"}}

    assert PersistentStorage.storage_key(user, "userID") == "user-1:userID"
    assert PersistentStorage.storage_key(user, "USER_ID") == "user-1:userID"
    assert PersistentStorage.storage_key(user, "stableID") == "stable-1:stableID"
    assert PersistentStorage.storage_key(user, "missingID") == ":missingID"
    assert PersistentStorage.storage_key(%User{user_id: nil}, "userID") == ":userID"
    assert PersistentStorage.storage_key(%User{user_id: "u"}, "stableID") == ":stableID"
  end
end
