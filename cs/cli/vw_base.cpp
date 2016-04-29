/*
Copyright (c) by respective owners including Yahoo!, Microsoft, and
individual contributors. All rights reserved.  Released under a BSD (revised)
license as described in the file LICENSE.
*/

#include "vw_clr.h"
#include "vw_base.h"
#include "vw_model.h"
#include "vw_prediction.h"
#include "vw_example.h"

#include "clr_io.h"
#include "clr_io_memory.h"
#include "vw_exception.h"
#include "parse_args.h"
#include "parse_regressor.h"

using namespace System;
using namespace System::Collections::Generic;
using namespace System::Text;

namespace VW
{
    VowpalWabbitBase::VowpalWabbitBase(VowpalWabbitSettings^ settings)
        : m_examples(nullptr), m_vw(nullptr), m_model(nullptr), m_settings(settings != nullptr ? settings : gcnew VowpalWabbitSettings), m_instanceCount(0)
    {
        m_examples = gcnew Bag<VowpalWabbitExample^>();
        if (m_settings->EnableThreadSafeExamplePooling)
            m_examples = Bag::Synchronized(m_examples);

        try
        {
            try
            {
                auto string = msclr::interop::marshal_as<std::string>(settings->Arguments);

                if (settings->Model != nullptr)
                {
                    m_model = settings->Model;
                    m_vw = VW::seed_vw_model(m_model->m_vw, string);
                    m_model->IncrementReference();
                }
                else
                {
                    if (settings->ModelStream == nullptr)
                    {
                        m_vw = VW::initialize(string);
                    }
                    else
                    {
                        clr_io_buf model(settings->ModelStream);
                        if (!settings->Arguments->Contains("--no_stdin"))
                          string += " --no_stdin";
                        m_vw = VW::initialize(string, &model);
                        settings->ModelStream->Close();
                    }
                }
            }
            catch (...)
            {
                // memory leak, but better than crashing
                m_vw = nullptr;
                throw;
            }
        }
        CATCHRETHROW
    }

    VowpalWabbitBase::~VowpalWabbitBase()
    {
        this->!VowpalWabbitBase();
    }

    VowpalWabbitBase::!VowpalWabbitBase()
    {
        if (m_instanceCount <= 0)
        {
            this->InternalDispose();
        }
    }

    void VowpalWabbitBase::IncrementReference()
    {
        // thread-safe increase of model reference counter
        System::Threading::Interlocked::Increment(m_instanceCount);
    }

    void VowpalWabbitBase::DecrementReference()
    {
        // thread-safe decrease of model reference counter
        if (System::Threading::Interlocked::Decrement(m_instanceCount) <= 0)
        {
            this->InternalDispose();
        }
    }

    void VowpalWabbitBase::InternalDispose()
    {
        if (m_vw != nullptr)
        {
            // de-allocate example pools that are managed for each even shared instances
            auto delete_prediction = m_vw->delete_prediction;
            auto delete_label = m_vw->p->lp.delete_label;

            if (m_examples != nullptr)
            {
                for each (auto ex in m_examples->RemoveAll())
                {
                    VW::dealloc_example(delete_label, *ex->m_example, delete_prediction);
                    ::free_it(ex->m_example);

                    // cleanup pointers in example chain
                    auto inner = ex;
                    while ((inner = inner->InnerExample) != nullptr)
                    {
                        inner->m_owner = nullptr;
                        inner->m_example = nullptr;
                    }

                    ex->m_example = nullptr;

                    // avoid that this example is returned again
                    ex->m_owner = nullptr;
                }

                m_examples = nullptr;
            }

            if (m_model != nullptr)
            {
                // this object doesn't own the VW instance
                m_model->DecrementReference();
                m_model = nullptr;
            }
        }

        try
        {
            if (m_vw != nullptr)
            {
                release_parser_datastructures(*m_vw);

                // make sure don't try to free m_vw twice in case VW::finish throws.
                vw* vw_tmp = m_vw;
                m_vw = nullptr;
                VW::finish(*vw_tmp);
            }

            // don't add code here as in the case of VW::finish throws an exception it won't be called
        }
        CATCHRETHROW
    }

    VowpalWabbitSettings^ VowpalWabbitBase::Settings::get()
    {
        return m_settings;
    }

    VowpalWabbitArguments^ VowpalWabbitBase::Arguments::get()
    {
        if (m_arguments == nullptr)
        {
            m_arguments = gcnew VowpalWabbitArguments(m_vw);
        }

        return m_arguments;
    }

    void VowpalWabbitBase::Reload([System::Runtime::InteropServices::Optional] String^ args)
    {
        if (m_settings->ParallelOptions != nullptr)
        {
            throw gcnew NotSupportedException("Cannot reload model if AllRecude is enabled.");
        }

        clr_io_memory_buf mem_buf;

        if (args == nullptr)
            args = String::Empty;

        auto stringArgs = msclr::interop::marshal_as<std::string>(args);

        try
        {
            VW::save_predictor(*m_vw, mem_buf);
            mem_buf.flush();

            release_parser_datastructures(*m_vw);

            // make sure don't try to free m_vw twice in case VW::finish throws.
            vw* vw_tmp = m_vw;
            m_vw = nullptr;
            VW::finish(*vw_tmp);

            // reload from model
            // seek to beginning
            mem_buf.reset_file(0);
            m_vw = VW::initialize(stringArgs.c_str(), &mem_buf);
        }
        CATCHRETHROW
    }

    String^ VowpalWabbitBase::AreFeaturesCompatible(VowpalWabbitBase^ other)
    {
        auto diff = VW::are_features_compatible(*m_vw, *other->m_vw);

        return diff == nullptr ? nullptr : gcnew String(diff);
    }

    String^ VowpalWabbitBase::ID::get()
    {
        return gcnew String(m_vw->id.c_str());
    }

    void VowpalWabbitBase::ID::set(String^ value)
    {
        m_vw->id = msclr::interop::marshal_as<std::string>(value);
    }

    void VowpalWabbitBase::SaveModel()
    {
      string name = m_vw->final_regressor_name;
      if (name.empty())
      {
        return;
      }

      // this results in extra marshaling but should be fine here
      this->SaveModel(gcnew String(name.c_str()));
    }

    void VowpalWabbitBase::SaveModel(String^ filename)
    {
      if (String::IsNullOrEmpty(filename))
        throw gcnew ArgumentException("Filename must not be null or empty");

      String^ directoryName = System::IO::Path::GetDirectoryName(filename);

      if (!String::IsNullOrEmpty(directoryName))
      {
        System::IO::Directory::CreateDirectory(directoryName);
      }

      auto name = msclr::interop::marshal_as<std::string>(filename);

      try
      {
        VW::save_predictor(*m_vw, name);
      }
      CATCHRETHROW
    }

    void VowpalWabbitBase::SaveModel(Stream^ stream)
    {
      if (stream == nullptr)
        throw gcnew ArgumentException("stream");

      try
      {
        VW::clr_io_buf buf(stream);

        VW::save_predictor(*m_vw, buf);
      }
      CATCHRETHROW
    }
}
