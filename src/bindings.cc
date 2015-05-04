// Copyright 2015, Camilo Aguilar
#include "bindings.h"

/* size of the event structure, not counting name */
#define EVENT_SIZE  (sizeof (struct inotify_event))

/* reasonable guess as to size of 1024 events */
#define BUF_LEN        (1024 * (EVENT_SIZE + 16))

namespace NodeInotify {
	static Persistent<String> path_sym;
	static Persistent<String> watch_for_sym;
	static Persistent<String> callback_sym;
	static Persistent<String> persistent_sym;
	static Persistent<String> watch_sym;
	static Persistent<String> mask_sym;
	static Persistent<String> cookie_sym;
	static Persistent<String> name_sym;

	void Inotify::Initialize(Handle<Object> exports) {
		Local<FunctionTemplate> t = NanNew<FunctionTemplate>(New);
		t->SetClassName(NanNew<String>("Inotify"));
		t->InstanceTemplate()->SetInternalFieldCount(1);

		NODE_SET_PROTOTYPE_METHOD(t, "addWatch", Inotify::AddWatch);
		NODE_SET_PROTOTYPE_METHOD(t, "removeWatch", Inotify::RemoveWatch);
		NODE_SET_PROTOTYPE_METHOD(t, "close", Inotify::Close);

		//Constants initialization
		NODE_DEFINE_CONSTANT(exports, IN_ACCESS); //File was accessed (read)
		NODE_DEFINE_CONSTANT(exports, IN_ATTRIB); //Metadata changed, e.g., permissions, timestamps,
													  //extended attributes, link count (since Linux 2.6.25),
													  //UID, GID, etc.
		NODE_DEFINE_CONSTANT(exports, IN_CLOSE_WRITE); //File opened for writing was closed
		NODE_DEFINE_CONSTANT(exports, IN_CLOSE_NOWRITE); //File not opened for writing was closed
		NODE_DEFINE_CONSTANT(exports, IN_CREATE); //File/directory created in watched directory
		NODE_DEFINE_CONSTANT(exports, IN_DELETE); //File/directory deleted from watched directory
		NODE_DEFINE_CONSTANT(exports, IN_DELETE_SELF); //Watched file/directory was itself deleted
		NODE_DEFINE_CONSTANT(exports, IN_MODIFY); //File was modified
		NODE_DEFINE_CONSTANT(exports, IN_MOVE_SELF); //Watched file/directory was itself moved
		NODE_DEFINE_CONSTANT(exports, IN_MOVED_FROM); //File moved out of watched directory
		NODE_DEFINE_CONSTANT(exports, IN_MOVED_TO); //File moved into watched directory
		NODE_DEFINE_CONSTANT(exports, IN_OPEN); //File was opened
		NODE_DEFINE_CONSTANT(exports, IN_IGNORED);// Watch was removed explicitly (inotify.watch.rm) or
											// automatically (file was deleted, or file system was
											// unmounted)
		NODE_DEFINE_CONSTANT(exports, IN_ISDIR); //Subject of this event is a directory
		NODE_DEFINE_CONSTANT(exports, IN_Q_OVERFLOW); //Event queue overflowed (wd is -1 for this event)
		NODE_DEFINE_CONSTANT(exports, IN_UNMOUNT); //File system containing watched object was unmounted
		NODE_DEFINE_CONSTANT(exports, IN_ALL_EVENTS);

		NODE_DEFINE_CONSTANT(exports, IN_ONLYDIR); // Only watch the path if it is a directory.
		NODE_DEFINE_CONSTANT(exports, IN_DONT_FOLLOW); // Do not follow a sym link
		NODE_DEFINE_CONSTANT(exports, IN_ONESHOT); // Only send event once
		NODE_DEFINE_CONSTANT(exports, IN_MASK_ADD); //Add (OR) events to watch mask for this pathname if it
											// already exists (instead of replacing mask).

		NODE_DEFINE_CONSTANT(exports, IN_CLOSE); // (IN_CLOSE_WRITE | IN_CLOSE_NOWRITE)  Close
		NODE_DEFINE_CONSTANT(exports, IN_MOVE);  //  (IN_MOVED_FROM | IN_MOVED_TO)  Moves

		NanAssignPersistent(path_sym, NanNew<String>("path"));
		NanAssignPersistent(watch_for_sym, NanNew<String>("watch_for"));
		NanAssignPersistent(callback_sym, NanNew<String>("callback"));
		NanAssignPersistent(persistent_sym, NanNew<String>("persistent"));
		NanAssignPersistent(watch_sym, NanNew<String>("watch"));
		NanAssignPersistent(mask_sym, NanNew<String>("mask"));
		NanAssignPersistent(cookie_sym, NanNew<String>("cookie"));
		NanAssignPersistent(name_sym, NanNew<String>("name"));

		Local<ObjectTemplate> object_tmpl = t->InstanceTemplate();
		object_tmpl->SetAccessor(persistent_sym, Inotify::GetPersistent);

		exports->Set(NanNew<String>("Inotify"), t->GetFunction());
	}

	Inotify::Inotify() : ObjectWrap() {
		//ev_init(&read_watcher, Inotify::Callback);
		read_watcher = new uv_poll_t;
		read_watcher->data = this;  //preserving my reference to use it inside Inotify::Callback
		//uv_poll_init(uv_default_loop(), &read_watcher, Inotify::Callback);
		persistent = true;
		poll_stopped = 0;
	}

	Inotify::Inotify(bool nonpersistent) : ObjectWrap() {
		read_watcher = new uv_poll_t;
		read_watcher->data = this; //preserving my reference so that we can use it inside Inotify::Callback
		//ev_init(&read_watcher, Inotify::Callback);
		persistent = nonpersistent;
		poll_stopped = 0;
	}

	Inotify::~Inotify() {
		if(!persistent) {
			//ev_ref(EV_DEFAULT_UC);
			uv_ref((uv_handle_t *) read_watcher);
		}
		//ev_io_stop(EV_DEFAULT_UC_ &read_watcher);

		// if Inotify::Close() was already called we do not need to
		// stop polling again thus it causes fail of assertion test
		StopPolling();

		//assert(!uv_is_pending(&read_watcher));
	}

	NAN_METHOD(Inotify::New) {
		NanEscapableScope();

		Inotify *inotify = NULL;
		if(args.Length() == 1 && args[0]->IsBoolean()) {
			inotify = new Inotify(args[0]->IsTrue());
		} else {
			inotify = new Inotify();
		}

		inotify->fd = inotify_init();

		if(inotify->fd == -1) {
			NanThrowError(NanNew<String>(strerror(errno)));
			NanReturnValue(NanNull());
		}

		int flags = fcntl(inotify->fd, F_GETFL);
		if(flags == -1) {
			flags = 0;
		}

		fcntl(inotify->fd, F_SETFL, flags | O_NONBLOCK);

		//ev_io_set(&inotify->read_watcher, inotify->fd, EV_READ);
		//ev_io_start(EV_DEFAULT_UC_ &inotify->read_watcher);
		uv_poll_init(uv_default_loop(), inotify->read_watcher, inotify->fd);
		uv_poll_start(inotify->read_watcher, UV_READABLE, Inotify::Callback);


		Local<Object> obj = args.This();
		inotify->Wrap(obj);

		if(!inotify->persistent) {
			//ev_unref(EV_DEFAULT_UC);
			uv_unref((uv_handle_t *) inotify->read_watcher);
		}
		/**
		* Increment object references to avoid be GCed while
		* we are waiting for events in the inotify file descriptor.
		* Also, the object is not weak anymore.
		*/
		inotify->Ref();

		return NanEscapeScope(obj);
	}

	NAN_METHOD(Inotify::AddWatch) {
		NanEscapableScope();
		uint32_t mask = 0;
		int watch_descriptor = 0;

		if(args.Length() < 1 || !args[0]->IsObject()) {
			return NanThrowTypeError("You must specify an object as first argument");
		}

		Local<Object> args_ = args[0]->ToObject();

		if(!args_->Has(path_sym)) {
			return NanThrowTypeError("You must specify a path to watch for events");
		}

		if(!args_->Has(callback_sym) ||
			!args_->Get(callback_sym)->IsFunction()) {
			return NanThrowTypeError("You must specify a callback function");
		}

		if(!args_->Has(watch_for_sym)) {
			mask |= IN_ALL_EVENTS;
		} else {
			if(!args_->Get(watch_for_sym)->IsInt32()) {
				return NanThrowTypeError("You must specify OR'ed set of events");
			}
			mask |= args_->Get(watch_for_sym)->Int32Value();
			if(mask == 0) {
				return NanThrowTypeError("You must specify OR'ed set of events");
			}
	   }

		String::Utf8Value path(args_->Get(path_sym));

		Inotify *inotify = ObjectWrap::Unwrap<Inotify>(args.This());

		/* add watch */
		watch_descriptor = inotify_add_watch(inotify->fd, (const char *) *path, mask);

		Local<Integer> descriptor = NanNew<Integer>(watch_descriptor);

		//Local<Function> callback = Local<Function>::Cast(args_->Get(callback_sym));
		inotify->handle_->Set(descriptor, args_->Get(callback_sym));

		return NanEscapeScope(descriptor);
	}

	NAN_METHOD(Inotify::RemoveWatch) {
		NanScope();
		uint32_t watch = 0;
		int ret = -1;

		if(args.Length() == 0 || !args[0]->IsInt32()) {
			return NanThrowTypeError("You must specify a valid watcher descriptor as argument");
		}
		watch = args[0]->Int32Value();

		Inotify *inotify = ObjectWrap::Unwrap<Inotify>(args.This());

		ret = inotify_rm_watch(inotify->fd, watch);
		if(ret == -1) {
			NanThrowError(NanNew<String>(strerror(errno)));
			NanReturnValue(NanFalse());
		}

		NanReturnValue(NanTrue());
	}

	void Inotify::on_handle_close(uv_handle_t* handle) {
		assert(!uv_is_active(handle));
		delete handle;
	}

	NAN_METHOD(Inotify::Close) {
		NanScope();
		int ret = -1;

		Inotify *inotify = ObjectWrap::Unwrap<Inotify>(args.This());
		ret = close(inotify->fd);

		if (ret == -1) {
			NanThrowError(NanNew<String>(strerror(errno)));
			NanReturnValue(NanFalse());
		}

		if (!inotify->persistent) {
			//ev_ref(EV_DEFAULT_UC);
			uv_ref((uv_handle_t *) inotify->read_watcher);
		}

		//ev_io_stop(EV_DEFAULT_UC_ &inotify->read_watcher);
		inotify->StopPolling();

		/*Eliminating reference created inside of Inotify::New.
		The object is also weak again.
		Now v8 can do its stuff and GC the object.
		*/
		inotify->Unref();

		NanReturnValue(NanTrue());
	}

	void Inotify::Callback(uv_poll_t *watcher, int status, int revents) {
		NanScope();

		Inotify *inotify = static_cast<Inotify*>(watcher->data);
		assert(watcher == inotify->read_watcher);

		char buffer[BUF_LEN];

		//int length = read(inotify->fd, buffer, BUF_LEN);

		Local<Value> argv[1];
		TryCatch try_catch;

		int i = 0;
		int sz = 0;
		while ((sz = read(inotify->fd, buffer, BUF_LEN)) > 0) {
			struct inotify_event *event;
			for (i = 0; i <= (sz-EVENT_SIZE); i += (EVENT_SIZE + event->len)) {
				event = (struct inotify_event *) &buffer[i];

				Local<Object> obj = NanNew<Object>();
				obj->Set(watch_sym, NanNew<Integer>(event->wd));
				obj->Set(mask_sym, NanNew<Integer>(event->mask));
				obj->Set(cookie_sym, NanNew<Integer>(event->cookie));

				if(event->len) {
					obj->Set(name_sym, NanNew<String>(event->name));
				}
				argv[0] = obj;

				inotify->Ref();
				Local<Value> callback_ = inotify->handle_->Get(NanNew<Integer>(event->wd));
				Local<Function> callback = Local<Function>::Cast(callback_);

				callback->Call(inotify->handle_, 1, argv);
				inotify->Unref();

				if(event->mask & IN_IGNORED) {
					//deleting callback because the watch was removed
					Local<Value> wd = NanNew<Integer>(event->wd);
					inotify->handle_->Delete(wd->ToString());
				}

				if (try_catch.HasCaught()) {
					FatalException(try_catch);
				}
			} // for
		} // while
	}

	NAN_GETTER(Inotify::GetPersistent) {
		Inotify *inotify = ObjectWrap::Unwrap<Inotify>(args.This());

		if (inotify->persistent) {
			NanReturnValue(NanTrue());
		}

		NanReturnValue(NanFalse());
	 }

	void Inotify::StopPolling() {
		if (!poll_stopped) {
			uv_poll_stop(read_watcher);
			uv_close((uv_handle_t *) read_watcher, Inotify::on_handle_close);
			poll_stopped = 1;
		}
	}

}//namespace NodeInotify
