defmodule Statsig.PersistentStorage.Reference do
  @moduledoc false
  @enforce_keys [:pid]
  defstruct [:pid]

  @type t :: %__MODULE__{pid: pid()}
end

defmodule Statsig.PersistentStorage do
  @moduledoc """
  Behaviour and helper APIs for receiving persistent-assignment (sticky value)
  updates from the Statsig SDK.

  Configuring `%Statsig.Options{persistent_storage: reference}` enables
  persistent assignment: `user_persisted_values` supplied via
  `Statsig.ExperimentEvaluationOptions` / `Statsig.LayerEvaluationOptions` are
  honored, and the SDK notifies this process when sticky values should be
  saved or deleted.

  Reads are NOT bridged through the SDK evaluation path: callers load values
  from their own store and pass them per call via the evaluation options.
  `get_values_for_user/3` is the caller-side helper for that read — it derives
  the storage key from the user and calls the implementation's `c:load/2`.
  `c:handle_save/4` receives the sticky values as a decoded map, matching the
  shape the SDK expects back in `user_persisted_values`.

  Storage keys have the format `"<unit id>:<id type>"` — see `storage_key/2`.

  Important: once persistent storage is configured, evaluating an experiment
  or layer with `user_persisted_values: nil` signals that the caller has no
  persisted values, and the SDK issues a `handle_delete` for that config.
  Always pass the loaded values (or an empty map for a user with nothing
  stored) when sticky assignment should stay active.

  Use `start_link/3` to launch the bridge process and pass the returned
  `%Statsig.PersistentStorage.Reference{}` into `Statsig.Options`.
  """

  alias Statsig.PersistentStorage.Reference
  alias Statsig.PersistentStorage.Server
  alias Statsig.User

  @typedoc "Opaque state returned from user callbacks."
  @type state :: term()

  @typedoc "Sticky values for one config, as a decoded map."
  @type sticky_values :: %{optional(String.t()) => term()}

  @typedoc "Map of config name to sticky values for one storage key."
  @type user_persisted_values :: %{optional(String.t()) => sticky_values()}

  @callback init(init_arg :: term()) :: {:ok, state()} | {:error, term()}

  @callback load(key :: String.t(), state()) ::
              {:ok, user_persisted_values() | nil, state()} | {:error, term()}

  @callback handle_save(
              key :: String.t(),
              config_name :: String.t(),
              sticky_values :: sticky_values(),
              state()
            ) :: {:ok, state()} | {:error, term()}

  @callback handle_delete(key :: String.t(), config_name :: String.t(), state()) ::
              {:ok, state()} | {:error, term()}

  @optional_callbacks handle_delete: 3

  @doc """
  Starts a bridge process for the provided implementation module.

  Returns `{:ok, %Statsig.PersistentStorage.Reference{}}` which can be
  assigned to `%Statsig.Options{persistent_storage: reference}`.
  """
  @spec start_link(module(), term(), Keyword.t()) :: {:ok, Reference.t()} | {:error, term()}
  def start_link(module, init_arg \\ nil, opts \\ []) do
    case Server.start_link(module, init_arg, opts) do
      {:ok, pid} -> {:ok, %Reference{pid: pid}}
      other -> other
    end
  end

  @doc """
  Loads the persisted values for a user, deriving the storage key via
  `storage_key/2` and calling the implementation's `c:load/2`.

  This is a synchronous call intended for the caller side of an evaluation:
  fetch the values here, then pass them as `user_persisted_values` in the
  experiment/layer evaluation options.
  """
  @spec get_values_for_user(Reference.t(), User.t(), String.t(), timeout()) ::
          {:ok, user_persisted_values() | nil} | {:error, term()}
  def get_values_for_user(
        %Reference{pid: pid},
        %User{} = user,
        id_type \\ "userID",
        timeout \\ 5_000
      ) do
    GenServer.call(pid, {:load, storage_key(user, id_type)}, timeout)
  end

  @doc """
  Derives the storage key for a user and ID type, in the format
  `"<unit id>:<id type>"` used by the SDK when issuing save/delete
  notifications.

  For `"userID"` (any casing, or `"user_id"`) the unit ID is the user's
  `user_id`; for any other ID type it is looked up in `custom_ids`. A missing
  unit ID yields an empty string, mirroring the other SDK bindings.
  """
  @spec storage_key(User.t(), String.t()) :: String.t()
  def storage_key(%User{} = user, id_type) when is_binary(id_type) do
    if String.downcase(id_type) in ["user_id", "userid"] do
      "#{user.user_id}:userID"
    else
      custom_id = Map.get(user.custom_ids || %{}, id_type) || ""
      "#{custom_id}:#{id_type}"
    end
  end

  @doc """
  Stops the bridge process for the given reference.
  """
  @spec stop(Reference.t(), term(), non_neg_integer()) :: :ok
  def stop(%Reference{pid: pid}, reason \\ :normal, timeout \\ 5_000) do
    GenServer.stop(pid, reason, timeout)
  end
end

defmodule Statsig.PersistentStorage.Server do
  @moduledoc false
  use GenServer
  require Logger

  @request_tag :statsig_persistent_storage_request

  @spec start_link(module(), term(), Keyword.t()) :: GenServer.on_start()
  def start_link(module, init_arg, opts) do
    GenServer.start_link(__MODULE__, %{module: module, init_arg: init_arg}, opts)
  end

  @impl true
  def init(%{module: module, init_arg: init_arg}) do
    case safe_apply(module, :init, [init_arg]) do
      {:ok, module_state} ->
        {:ok, %{module: module, module_state: module_state}}

      {:error, reason} ->
        {:stop, reason}

      other ->
        {:stop, {:bad_return, {module, :init, other}}}
    end
  end

  @impl true
  def handle_call({:load, key}, _from, state) do
    case safe_apply(state.module, :load, [key, state.module_state]) do
      {:ok, values, new_module_state} ->
        {:reply, {:ok, values}, %{state | module_state: new_module_state}}

      {:error, reason} ->
        {:reply, {:error, reason}, state}

      other ->
        {:reply, {:error, {:bad_return, {state.module, :load, other}}}, state}
    end
  end

  @impl true
  def handle_info({@request_tag, :save, key, config_name, sticky_values}, state) do
    {:noreply, dispatch(state, :handle_save, [key, config_name, sticky_values])}
  end

  @impl true
  def handle_info({@request_tag, :delete, key, config_name}, state) do
    {:noreply, dispatch(state, :handle_delete, [key, config_name])}
  end

  @impl true
  def handle_info(message, state) do
    Logger.debug(
      "Statsig.PersistentStorage.Server received unexpected message: #{inspect(message)}"
    )

    {:noreply, state}
  end

  # Save/delete notifications are fire-and-forget from the native side: the
  # SDK captures this process's pid once at initialization, so a callback that
  # raises must never crash the bridge — a supervised restart would leave the
  # SDK messaging a dead pid and silently disable persistence.
  defp dispatch(state, callback, args) do
    arity = length(args) + 1

    if function_exported?(state.module, callback, arity) do
      case safe_apply(state.module, callback, args ++ [state.module_state]) do
        {:ok, new_module_state} ->
          %{state | module_state: new_module_state}

        {:error, reason} ->
          Logger.warning(
            "Statsig.PersistentStorage #{callback} failed for #{inspect(state.module)}: #{inspect(reason)}"
          )

          state

        other ->
          Logger.warning(
            "Statsig.PersistentStorage #{callback} returned unexpected value: #{inspect(other)}"
          )

          state
      end
    else
      state
    end
  end

  defp safe_apply(module, fun, args) do
    try do
      apply(module, fun, args)
    rescue
      exception -> {:error, Exception.message(exception)}
    catch
      kind, reason -> {:error, {kind, reason}}
    end
  end
end
